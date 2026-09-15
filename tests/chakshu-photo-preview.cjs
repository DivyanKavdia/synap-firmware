'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('SD preview preserves the original exposure and falls back safely on decode, encode or memory failures',()=>{
 const source=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
 const preview=source.slice(source.indexOf('struct PreviewJpeg'),source.indexOf('uint8_t catalogue()'));
 assert.match(nativeTest(`#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace ChakshuMedia {enum {NO_SD=3,NO_SPACE=6,IO_ERROR=7};}
uint8_t* buffer=nullptr;size_t bufferSize=0;char originalPath[64]{};
std::vector<uint8_t> original(8000,91),saved;
int captures=0,allocations=0,failAllocation=0,decodes=0;
bool writeFails=false,decodeFails=false,encodeFails=false,overflow=false,badDimensions=false;
void* ps_malloc(size_t n){return ++allocations==failAllocation?nullptr:malloc(n);}
uint8_t captureFrame(bool preview){assert(!preview);++captures;free(buffer);bufferSize=original.size();buffer=static_cast<uint8_t*>(malloc(bufferSize));memcpy(buffer,original.data(),bufferSize);return 0;}
struct File {explicit operator bool()const{return true;}size_t write(const uint8_t* b,size_t n){saved.assign(b,b+n);return writeFails?0:n;}void flush(){}void close(){}};
namespace ChakshuStorage {bool ready=true;uint64_t freeBytes=8000000;constexpr uint32_t RESERVE_BYTES=4000000;void refresh(){}
 File create(char* path,size_t n,const char* ext){assert(!strcmp(ext,"jpg"));snprintf(path,n,"/synap/abcdef01-00000001.jpg");return {};}
}
constexpr int ESP_OK=0,JPEG_IMAGE_FORMAT_RGB565=2,JPEG_IMAGE_SCALE_1_2=1,PIXFORMAT_RGB565=3;
struct esp_jpeg_image_cfg_t {uint8_t* indata=nullptr;size_t indata_size=0;uint8_t* outbuf=nullptr;size_t outbuf_size=0;int out_format=0,out_scale=0;struct{bool swap_color_bytes=false;}flags;};
struct esp_jpeg_image_output_t {unsigned width=0,height=0;size_t output_len=0;};
int esp_jpeg_get_image_info(esp_jpeg_image_cfg_t* c,esp_jpeg_image_output_t* i){
 assert(c->indata==buffer&&c->indata_size==original.size()&&c->outbuf_size==153600);
 assert(c->out_format==JPEG_IMAGE_FORMAT_RGB565&&c->out_scale==JPEG_IMAGE_SCALE_1_2&&c->flags.swap_color_bytes);
 i->width=badDimensions?640:320;i->height=240;i->output_len=153600;return ESP_OK;
}
int esp_jpeg_decode(esp_jpeg_image_cfg_t* c,esp_jpeg_image_output_t*){++decodes;memset(c->outbuf,0,c->outbuf_size);return decodeFails?-1:ESP_OK;}
bool fmt2jpg_cb(uint8_t*,size_t size,unsigned w,unsigned h,int format,int quality,size_t(*callback)(void*,size_t,const void*,size_t),void* arg){
 assert(size==153600&&w==320&&h==240&&format==PIXFORMAT_RGB565&&quality==60);
 const uint8_t jpeg[]={255,216,1,2,3,4,255,217};
 if(overflow)callback(arg,64000,jpeg,sizeof(jpeg));else callback(arg,0,jpeg,sizeof(jpeg));
 return !encodeFails;
}
${preview}
void reset(){ChakshuStorage::ready=true;ChakshuStorage::freeBytes=8000000;writeFails=decodeFails=encodeFails=overflow=badDimensions=false;failAllocation=0;allocations=0;saved.clear();}
int main(){
 assert(captureSavedPreview()==0&&captures==1&&bufferSize==8&&saved==original&&originalPath[0]);
 for(int mode=0;mode<6;mode++){
  reset();decodeFails=mode==0;encodeFails=mode==1;overflow=mode==2;badDimensions=mode==3;failAllocation=mode==4?1:mode==5?2:0;
  const int before=decodes;assert(captureSavedPreview()==0&&bufferSize==original.size()&&saved==original);
  assert(!memcmp(buffer,original.data(),original.size()));if(badDimensions)assert(decodes==before);
 }
 reset();writeFails=true;assert(captureSavedPreview()==7&&!originalPath[0]&&!ChakshuStorage::ready);
 reset();ChakshuStorage::freeBytes=4000000;assert(captureSavedPreview()==6&&saved.empty());
 reset();ChakshuStorage::ready=false;const int before=captures;assert(captureSavedPreview()==3&&captures==before);
 free(buffer);puts("PASS same-exposure SD originals and bounded color-correct preview fallback");
}`),/PASS same-exposure SD originals/);
});
