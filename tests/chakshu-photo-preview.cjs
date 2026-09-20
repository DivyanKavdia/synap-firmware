'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('SD preview preserves the full-quality original and falls back safely on preview failures',()=>{
 const source=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
 const preview=source.slice(source.indexOf('struct PreviewJpeg'),source.indexOf('uint8_t catalogue()'));
 assert.match(nativeTest(`#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace ChakshuMedia {enum {OK=0,NO_SD=3,NO_SPACE=6,IO_ERROR=7,CAPTURE_ERROR=8};}
uint8_t* buffer=nullptr;size_t bufferSize=0;char originalPath[64]{};
std::vector<uint8_t> original(8000,91),saved;
int allocations=0,failAllocation=0,decodes=0,frameGets=0,restores=0;
bool writeFails=false,decodeFails=false,encodeFails=false,overflow=false,badDimensions=false,beginFails=false;
void* ps_malloc(size_t n){return ++allocations==failAllocation?nullptr:malloc(n);}
void clearSelection();
constexpr int PIXFORMAT_JPEG=7;
struct camera_fb_t {int format=PIXFORMAT_JPEG;size_t len=0;uint8_t* buf=nullptr;} frame;
camera_fb_t* esp_camera_fb_get(){++frameGets;frame.len=original.size();frame.buf=original.data();return &frame;}
void esp_camera_fb_return(camera_fb_t* f){assert(f==&frame);}
namespace ChakshuCamera {
 bool beginOriginal(){return !beginFails;}
 void endOriginal(){++restores;}
}
struct File {
 explicit operator bool()const{return true;}
 size_t write(const uint8_t* b,size_t n){saved.assign(b,b+n);return writeFails?0:n;}
 void flush(){}void close(){}
};
namespace ChakshuStorage {
 bool ready=true;uint64_t freeBytes=8000000;constexpr uint32_t RESERVE_BYTES=4000000;bool protectedCapture=false;
 void refresh(){}
 bool ensureSpace(uint64_t expected=0){return ready&&freeBytes>=uint64_t(RESERVE_BYTES)+expected;}
 void protect(const char*){protectedCapture=true;}
 void clearProtection(){protectedCapture=false;}
 File create(char* path,size_t n,const char* ext){assert(!strcmp(ext,"jpg"));snprintf(path,n,"/synap/abcdef01-00000001.jpg");return {};}
}
void clearSelection(){ChakshuStorage::clearProtection();free(buffer);buffer=nullptr;bufferSize=0;}
constexpr int ESP_OK=0,JPEG_IMAGE_FORMAT_RGB565=2,JPEG_IMAGE_SCALE_1_8=3,PIXFORMAT_RGB565=4;
struct esp_jpeg_image_cfg_t {uint8_t* indata=nullptr;size_t indata_size=0;uint8_t* outbuf=nullptr;size_t outbuf_size=0;int out_format=0,out_scale=0;struct{bool swap_color_bytes=false;}flags;};
struct esp_jpeg_image_output_t {unsigned width=0,height=0;size_t output_len=0;};
int esp_jpeg_get_image_info(esp_jpeg_image_cfg_t* c,esp_jpeg_image_output_t* i){
 assert(c->indata==buffer&&c->indata_size==original.size());
 assert(c->out_format==JPEG_IMAGE_FORMAT_RGB565&&c->out_scale==JPEG_IMAGE_SCALE_1_8&&c->flags.swap_color_bytes);
 i->width=badDimensions?640:256;i->height=192;i->output_len=size_t(i->width)*i->height*2;return ESP_OK;
}
int esp_jpeg_decode(esp_jpeg_image_cfg_t* c,esp_jpeg_image_output_t* i){
 ++decodes;assert(c->outbuf&&c->outbuf_size==98304);
 if(decodeFails)return -1;
 memset(c->outbuf,0,c->outbuf_size);i->width=256;i->height=192;i->output_len=98304;return ESP_OK;
}
bool fmt2jpg_cb(uint8_t*,size_t size,unsigned w,unsigned h,int format,int quality,size_t(*callback)(void*,size_t,const void*,size_t),void* arg){
 assert(size==98304&&w==256&&h==192&&format==PIXFORMAT_RGB565&&quality==70);
 const uint8_t jpeg[]={255,216,1,2,3,4,255,217};
 if(overflow)callback(arg,95999,jpeg,sizeof(jpeg));else callback(arg,0,jpeg,sizeof(jpeg));
 return !encodeFails;
}
${preview}
void reset(){
 ChakshuStorage::ready=true;ChakshuStorage::freeBytes=8000000;ChakshuStorage::protectedCapture=false;
 writeFails=decodeFails=encodeFails=overflow=badDimensions=beginFails=false;failAllocation=0;allocations=0;saved.clear();
 clearSelection();originalPath[0]=0;
}
int main(){
 assert(captureSavedPreview()==0&&frameGets==2&&bufferSize==8&&saved==original&&originalPath[0]);
 assert(!memcmp(saved.data(),original.data(),original.size())&&!ChakshuStorage::protectedCapture&&restores==1);
 for(int mode=0;mode<6;mode++){
  reset();decodeFails=mode==0;encodeFails=mode==1;overflow=mode==2;badDimensions=mode==3;
  failAllocation=mode==4?2:mode==5?3:0;
  const int before=decodes;
  assert(captureSavedPreview()==0&&bufferSize==original.size()&&saved==original&&originalPath[0]);
  assert(!memcmp(buffer,original.data(),original.size())&&!ChakshuStorage::protectedCapture);
  if(badDimensions)assert(decodes==before);
 }
 reset();failAllocation=1;assert(captureSavedPreview()==8&&saved.empty()&&!buffer&&!ChakshuStorage::protectedCapture);
 reset();writeFails=true;assert(captureSavedPreview()==7&&!originalPath[0]&&!ChakshuStorage::ready&&!buffer&&!ChakshuStorage::protectedCapture);
 reset();ChakshuStorage::freeBytes=4000000;assert(captureSavedPreview()==6&&saved.empty()&&!buffer);
 reset();beginFails=true;assert(captureSavedPreview()==8&&saved.empty()&&!buffer);
 reset();ChakshuStorage::ready=false;const int before=frameGets;assert(captureSavedPreview()==3&&frameGets==before);
 free(buffer);puts("PASS full-quality SD originals and bounded color-correct preview fallback");
}`),/PASS full-quality SD originals/);
});


test('voice photo reuses hardened saved-photo path and never races live audio streaming',()=>{
 const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
 const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');
 const photoCase=transfer.slice(transfer.indexOf('case 11:'),transfer.indexOf('case 2:case 4:'));
 assert.match(photoCase,/if\(streamingEnabled\.load\(\)\)error=ChakshuMedia::BUSY/);
 assert.match(photoCase,/error=captureSavedPreview\(\)/);
 assert.doesNotMatch(photoCase,/file\.write\(frame->buf/);
 const saved=transfer.slice(transfer.indexOf('uint8_t captureSavedPreview\(\)'),transfer.indexOf('uint8_t catalogue\(\)'));
 assert(saved.indexOf('ChakshuCamera::endOriginal();') < saved.indexOf('ChakshuStorage::ensureSpace(bufferSize)'));
 assert.match(saved,/if\(!saved\)\{ChakshuStorage::ready=false/);
 assert.match(transfer,/if\(error==ChakshuMedia::IO_ERROR\|\|error==ChakshuMedia::NO_SD\)\s*\{\s*ChakshuStorage::ready=false/);
 const commands=voice.slice(voice.indexOf('if(command==STOP)'),voice.indexOf('portENTER_CRITICAL(&stateMux)',voice.indexOf('if(command==STOP)')));
 assert.match(commands,/command==PHOTO[\s\S]*streamingEnabled\.load\(\)/);
 assert.match(commands,/command==VIDEO_START[\s\S]*streamingEnabled\.load\(\)/);
});
