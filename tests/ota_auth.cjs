// Host-only test of the actual firmware authorize method; OpenSSL substitutes mbedTLS.
// Requires Node, g++, OpenSSL development headers/library. Does not compile the ESP image.
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),crypto=require('node:crypto');
const {execFileSync}=require('node:child_process');
const source=fs.readFileSync(path.join(__dirname,'../firmware/dk_pendant_esp32s3/SynapOTA.h'),'utf8');
const method=source.slice(source.indexOf('  Synap::OtaError authorize('),source.indexOf('  bool begin(')).replace(' override','');
if(!method.includes('otaRefreshChallenge()')) throw new Error('Authorization method extraction failed');
const members=source.slice(source.indexOf('  bool attempted='),source.indexOf('  esp_ota_handle_t handle='));
const domain=source.match(/static const char OTA_AUTH_DOMAIN\[\] = "([^"]+)";/)[1];
const metadata=Buffer.alloc(41);metadata[0]=1;metadata.writeUInt32LE(7,1);metadata.writeUInt32LE(512,5);metadata.fill(9,9);
const key=Buffer.alloc(32,0xa1),nonce=Buffer.alloc(16,8);
const mac=crypto.createHmac('sha256',key).update(domain).update(nonce).update(metadata).digest();
const array=b=>Array.from(b).join(',');
const program=`
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <openssl/hmac.h>
#include <iostream>
namespace Synap {enum OtaError {OK,AUTH_FAILED,AUTH_THROTTLED};}
static const char OTA_AUTH_DOMAIN[]="${domain}";
uint8_t otaOwnerKey[32],otaChallenge[16];
int refreshes=0;
void otaRefreshChallenge(){++refreshes;for(auto& b:otaChallenge)++b;}
constexpr int MBEDTLS_MD_SHA256=1;
const EVP_MD* mbedtls_md_info_from_type(int){return EVP_sha256();}
int mbedtls_md_hmac(const EVP_MD* md,const uint8_t* key,size_t kl,const uint8_t* msg,size_t ml,uint8_t* out){
  unsigned n=0;return HMAC(md,key,int(kl),msg,ml,out,&n) && n==32 ? 0 : -1;
}
class Auth {public:
${method}
private:
${members}
};
uint8_t metadata[]={${array(metadata)}},mac[]={${array(mac)}};
void resetNonce(){memset(otaChallenge,8,16);}
int main(){
 memset(otaOwnerKey,0xa1,32);resetNonce();Auth auth;
 assert(auth.authorize(metadata,mac,0)==Synap::OK);
 assert(refreshes==1);
 assert(auth.authorize(metadata,mac,999)==Synap::AUTH_THROTTLED);
 assert(refreshes==1); // Throttled attempts do not consume a challenge.
 assert(auth.authorize(metadata,mac,1000)==Synap::AUTH_FAILED); // Replay.
 for(int offset:{0,1,5,9,40}) {resetNonce();Auth test;metadata[offset]^=1;
   assert(test.authorize(metadata,mac,0)==Synap::AUTH_FAILED);metadata[offset]^=1;}
 resetNonce();Auth wrong;otaOwnerKey[0]^=1;
 assert(wrong.authorize(metadata,mac,0)==Synap::AUTH_FAILED);otaOwnerKey[0]^=1;
 Auth brute;uint8_t bad[32]{};
 for(unsigned i=0;i<5;++i) assert(brute.authorize(metadata,bad,1000*i)==Synap::AUTH_FAILED);
 assert(brute.authorize(metadata,bad,33999)==Synap::AUTH_THROTTLED);
 resetNonce();assert(brute.authorize(metadata,mac,34000)==Synap::OK);
 Auth wrap;resetNonce();assert(wrap.authorize(metadata,mac,UINT32_MAX-100)==Synap::OK);
 assert(wrap.authorize(metadata,mac,100)==Synap::AUTH_THROTTLED);
 resetNonce();assert(wrap.authorize(metadata,mac,900)==Synap::OK);
 std::cout<<"PASS: firmware HMAC matches Node reference; metadata binding, replay, wrong key, cooldown and clock wrap\\n";
}
`;
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'synap-auth-test-'));
const cpp=path.join(dir,'auth.cpp'),bin=path.join(dir,'auth');
try {
  fs.writeFileSync(cpp,program);
  execFileSync('g++',['-std=c++17','-Wall','-Wextra','-Werror',cpp,'-lcrypto','-o',bin],{stdio:'inherit'});
  execFileSync(bin,[],{stdio:'inherit'});
} finally { for(const file of [cpp,bin]) if(fs.existsSync(file)) fs.unlinkSync(file);fs.rmdirSync(dir); }
