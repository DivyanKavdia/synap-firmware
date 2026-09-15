'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync('firmware/xiao-sense/wifi-downloads.cpp','utf8');
test('private download handler rejects untrusted paths and tokens and bounds transfer ownership',()=>{
 const guards=source.slice(source.indexOf('bool validPath('),source.indexOf('void page()'));
 const download=source.slice(source.indexOf('void file()'),source.indexOf('void end()'));
 assert.match(nativeTest(`#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
struct String:std::string {
 using std::string::string;String(const std::string& s):std::string(s){}
 bool startsWith(const char* s)const{return rfind(s,0)==0;}bool endsWith(const char* s)const{return size()>=strlen(s)&&compare(size()-strlen(s),strlen(s),s)==0;}
 String substring(size_t n)const{return substr(n);}
};
constexpr int FILE_READ=0;constexpr uint32_t MAX_MS=900000;
uint32_t now=0,started=0,lastActivity=0;uint32_t millis(){return now;}void vTaskDelay(int){++now;}
std::atomic<bool> stopRequested{false};
struct Info {bool active=true;String token="private-token";} info;
Info copy(){return info;}
std::vector<uint8_t> bytes(10000,73);unsigned opens=0;bool cardAvailable=true;
struct File {bool valid=true;size_t offset=0;explicit operator bool()const{return valid;}bool isDirectory(){return false;}void close(){}size_t size(){return bytes.size();}
 size_t read(uint8_t* out,size_t n){n=std::min(n,bytes.size()-offset);memcpy(out,bytes.data()+offset,n);offset+=n;return n;}
};
struct Storage {File open(const String& path,int mode){assert(mode==FILE_READ&&path=="/synap/12345678-abcdef01.jpg");++opens;return {cardAvailable,0};}} SD;
struct Client {std::vector<uint8_t> sent;bool connected(){return true;}bool stopAfterFirst=false,fail=false,stopped=false;
 size_t write(const uint8_t* p,size_t n){if(fail)return 0;sent.insert(sent.end(),p,p+n);if(stopAfterFirst)stopRequested=true;return n;}void stop(){stopped=true;}
};
struct Server {std::map<String,String> args,headers;int code=0;size_t length=0;Client network;
 String arg(const char* key){return args[key];}void sendHeader(const String& key,const String& value){headers[key]=value;}
 void send(int status,const char*,const char*){code=status;}void setContentLength(size_t n){length=n;}Client& client(){return network;}
} implementation;
auto* server=&implementation;
${guards}
${download}
int main(){
 for(const char* p:{"/synap/12345678-abcdef01.jpg","/synap/12345678-abcdef01.mjpeg","/synap/12345678-abcdef01.wav","/synap/12345678-abcdef01.json"})assert(validPath(p));
 for(const char* p:{"/synap/../../secret","/synap/12345678-abcdef01.jpg/..","/synap/12345678-abcdef01.JPG","/synap/12345678-abcdef01.txt","/synap/12345678-abcdef01.jpg?x","/synap/12345678-abcdef01..jpg","/synap/12345678-abcdef01.'.jpg"})assert(!validPath(p));
 server->args["path"]="/synap/12345678-abcdef01.jpg";
 file();assert(server->code==403 && opens==0);server->args["key"]="private-token";
 file();assert(server->code==200 && opens==1 && server->network.sent==bytes && server->length==bytes.size());
 assert(server->headers["Cache-Control"]=="no-store"&&server->headers["Referrer-Policy"]=="no-referrer");
 server->network={};server->network.stopAfterFirst=true;file();assert(server->network.sent.size()==4096&&server->network.stopped);
 stopRequested=false;server->network={};server->network.fail=true;file();assert(server->network.stopped&&server->network.sent.empty());
 server->network={};now=MAX_MS;file();assert(server->network.stopped&&server->network.sent.empty());
 now=0;cardAvailable=false;file();assert(server->code==404);
 info.active=false;file();assert(server->code==403);
 puts("PASS private read-only download, path checks, Stop, timeout and failure");
}`),/PASS private read-only download/);
});
