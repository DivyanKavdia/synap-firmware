'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),zlib=require('node:zlib');
const {execFileSync}=require('node:child_process');
const {materialize}=require('../tools/materialize-target.cjs');
const {embeddedBytes,verify}=require('../tools/verify-embedded-model.cjs');
const shared=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const target='xiao-esp32s3-sense-8m';
test('embedded model tooling refuses C3/S3 and corrupt Chakshu weights before changing source',()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'chakshu-pack-'));
  try {
    const model=path.join(dir,'bad.bin');fs.writeFileSync(model,Buffer.alloc(20));
    for(const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m',target]){
      const source=materialize(shared,id),sketch=path.join(dir,'sketch.ino');fs.writeFileSync(sketch,source);
      assert.throws(()=>execFileSync('python3',['tools/embed-voice-model.py',sketch,model],{stdio:'pipe'}));
      assert.equal(fs.readFileSync(sketch,'utf8'),source);
      if(id!==target)assert(!source.includes('ChakshuFlashModel'));
    }
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
});
test('pinned model round-trips through prepared source, is required in linked image and fits the OTA budget',
  {skip:!process.env.SYNAP_MODEL_PACK},()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'chakshu-pack-'));
  try {
    const model=path.resolve(process.env.SYNAP_MODEL_PACK),weights=fs.readFileSync(model),sketch=path.join(dir,'sketch.ino');
    fs.writeFileSync(sketch,materialize(shared,target));
    execFileSync('python3',['tools/embed-voice-model.py',sketch,model]);
    const source=fs.readFileSync(sketch,'utf8'),packed=embeddedBytes(source);
    assert(packed.length<1600000);assert(zlib.inflateRawSync(packed).equals(weights));
    const image=Buffer.concat([Buffer.alloc(1300000),packed]);verify(image,source,weights);
    assert.throws(()=>verify(Buffer.alloc(1300000),source,weights),/missing/);
    assert.throws(()=>verify(Buffer.concat([image,Buffer.alloc(700000)]),source,weights),/OTA slot/);
    assert.throws(()=>execFileSync('python3',['tools/embed-voice-model.py',sketch,model],{stdio:'pipe'}));
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
});
test('bounded flash loader decompresses real data and rejects truncation, overflow and allocation failure',()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'chakshu-inflate-'));
  try {
    const source=fs.readFileSync('firmware/xiao-sense/model-flash.cpp','utf8');
    const loader=source.slice(source.indexOf('LoadResult inflate('),source.indexOf('\n#endif\nLoadResult load('));
    // zlib supplies a host streaming decoder with the ROM tinfl call contract.
    // ESP32 CI separately compiles and links the real ROM decoder.
    const bytes=process.env.SYNAP_MODEL_PACK?fs.readFileSync(process.env.SYNAP_MODEL_PACK):Buffer.alloc(2177224);
    if(!process.env.SYNAP_MODEL_PACK)for(let i=0;i<bytes.length;i++)bytes[i]=(i*17+(i>>10))&255;
    const packed=zlib.deflateRawSync(bytes);fs.writeFileSync(path.join(dir,'raw'),bytes);fs.writeFileSync(path.join(dir,'packed'),packed);
    const cpp=`#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include <zlib.h>
namespace ChakshuModel {constexpr size_t MODEL_BYTES=2177224;}
enum LoadResult {LOADED,MISSING,NO_MEMORY,INVALID};
struct tinfl_decompressor {z_stream stream;};
constexpr int TINFL_STATUS_DONE=0,TINFL_STATUS_HAS_MORE_OUTPUT=2,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF=4;
bool allocationFails=false;int allocations=0,yields=0;
void* allocState(size_t a,size_t b){if(allocationFails)return nullptr;++allocations;return calloc(a,b);}
void releaseState(void* p){if(p){inflateEnd(&static_cast<tinfl_decompressor*>(p)->stream);--allocations;free(p);}}
void tinfl_init(tinfl_decompressor* p){assert(inflateInit2(&p->stream,-15)==Z_OK);}
int tinfl_decompress(tinfl_decompressor* p,const uint8_t* input,size_t* inputSize,uint8_t*,uint8_t* output,size_t* outputSize,int flags){
 assert(flags==4);auto& s=p->stream;s.next_in=const_cast<uint8_t*>(input);s.avail_in=*inputSize;s.next_out=output;s.avail_out=*outputSize;
 const int status=::inflate(&s,Z_NO_FLUSH);*inputSize-=s.avail_in;*outputSize-=s.avail_out;
 return status==Z_STREAM_END?0:status==Z_OK&&s.avail_out==0?2:-1;
}
void vTaskDelay(int){++yields;}
#define calloc allocState
#define free releaseState
${loader}
#undef calloc
#undef free
std::vector<uint8_t> read(const char* file){std::ifstream f(file,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){assert(argc==3);auto input=read(argv[1]),expected=read(argv[2]);
 std::vector<uint8_t> output(expected.size()+2,0xAA);auto* out=output.data()+1;
 assert(inflate(input.data(),input.size(),out,expected.size())==LOADED);
 assert(!memcmp(out,expected.data(),expected.size())&&output.front()==0xAA&&output.back()==0xAA&&yields>0&&allocations==0);
 assert(inflate(input.data(),input.size()-4,out,expected.size())==INVALID&&allocations==0);
 input.push_back(0);assert(inflate(input.data(),input.size(),out,expected.size())==INVALID&&allocations==0);input.pop_back();
 assert(inflate(input.data(),input.size(),out,expected.size()-1)==INVALID);
 allocationFails=true;assert(inflate(input.data(),input.size(),out,expected.size())==NO_MEMORY&&allocations==0);
 allocationFails=false;input[0]=0x07;assert(inflate(input.data(),input.size(),out,expected.size())==INVALID&&allocations==0);
 assert(output.front()==0xAA&&output.back()==0xAA);
}`;
    const file=path.join(dir,'test.cpp'),binary=path.join(dir,'test');fs.writeFileSync(file,cpp);
    execFileSync('g++',['-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined',file,'-lz','-o',binary]);
    execFileSync(binary,[path.join(dir,'packed'),path.join(dir,'raw')],{timeout:20000});
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
});
