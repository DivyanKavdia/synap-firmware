'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('camera mode changes preserve VGA photos and reject failed sensor changes without leaking a frame',()=>{
 const camera=fs.readFileSync('firmware/xiao-sense/camera.cpp','utf8');
 const config=camera.slice(camera.indexOf('framesize_t frameSize='),camera.indexOf('// Allocate at'));
 const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
 const capture=transfer.slice(transfer.indexOf('uint8_t captureFrame('),transfer.indexOf('struct PreviewJpeg'));
 const fixture=`#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
using framesize_t=int;constexpr int FRAMESIZE_QVGA=1,FRAMESIZE_VGA=2,FRAMESIZE_HD=3,FRAMESIZE_UXGA=4,FRAMESIZE_QXGA=5,PIXFORMAT_JPEG=7;
int changes=0,mode=FRAMESIZE_VGA,returns=0,gets=0;bool sensorPresent=true,changeFails=false,allocateFails=false,getFails=false;
struct sensor_t {int (*set_framesize)(sensor_t*,framesize_t);int (*set_quality)(sensor_t*,int);};
int change(sensor_t*,framesize_t next){++changes;if(changeFails)return -1;mode=next;return 0;}
int quality=12;int setQuality(sensor_t*,int q){quality=q;return 0;}
sensor_t sensor{change,setQuality};sensor_t* esp_camera_sensor_get(){return sensorPresent?&sensor:nullptr;}
namespace ChakshuCamera {bool ready=true;uint16_t sensorPid=0x3660;${config}}
namespace ChakshuMedia {enum {OK=0,NO_CAMERA=4,CAPTURE_ERROR=8};}
uint8_t jpeg[]={0xff,0xd8,3,4,5,0xff,0xd9};
struct camera_fb_t {int format;size_t len;uint8_t* buf;} frame{PIXFORMAT_JPEG,sizeof(jpeg),jpeg};
camera_fb_t* esp_camera_fb_get(){++gets;return getFails?nullptr:&frame;}
void esp_camera_fb_return(camera_fb_t* p){assert(p==&frame);++returns;}
void* ps_malloc(size_t bytes){return allocateFails?nullptr:malloc(bytes);}
uint8_t* buffer=nullptr;size_t bufferSize=0;void clearSelection(){free(buffer);buffer=nullptr;bufferSize=0;}
${capture}
int main(){
 assert(captureFrame(true)==0&&mode==FRAMESIZE_QVGA&&changes==1&&returns==2&&quality==22);
 assert(bufferSize==sizeof(jpeg)&&memcmp(buffer,jpeg,sizeof(jpeg))==0&&buffer!=jpeg);
 assert(captureFrame(true)==0&&changes==1&&returns==4);
 assert(captureFrame()==0&&mode==FRAMESIZE_VGA&&changes==2&&returns==6&&quality==12);
 changeFails=true;assert(captureFrame(true)==8&&!buffer&&!bufferSize&&returns==6);changeFails=false;
 sensorPresent=false;assert(captureFrame(true)==8&&returns==6);sensorPresent=true;
 allocateFails=true;assert(captureFrame(true)==8&&!buffer&&returns==8);allocateFails=false;
 frame.format=0;assert(captureFrame()==8&&!buffer&&returns==10);frame.format=PIXFORMAT_JPEG;
 getFails=true;assert(captureFrame()==8&&!buffer);getFails=false;
 ChakshuCamera::ready=false;const int before=gets;assert(captureFrame()==4&&gets==before);
 clearSelection();puts("PASS photo/video sensor modes, complete JPEG ownership and failures");
}`;
 assert.match(nativeTest(fixture),/PASS photo\/video sensor modes/);
});

test('SD camera allocates at full resolution and restores the single-buffer phone camera, including failures',()=>{
 const source=fs.readFileSync('firmware/xiao-sense/camera.cpp','utf8').replace('#include <esp_camera.h>','');
 const fixture=`#include <cassert>
#include <cstdint>
#include <cstdio>
using framesize_t=int;using esp_err_t=int;
constexpr int FRAMESIZE_QVGA=1,FRAMESIZE_VGA=2,FRAMESIZE_HD=3,FRAMESIZE_UXGA=4,FRAMESIZE_QXGA=5,PIXFORMAT_JPEG=7,ESP_OK=0,LEDC_CHANNEL_0=0,LEDC_TIMER_0=0,CAMERA_GRAB_LATEST=1,CAMERA_GRAB_WHEN_EMPTY=0,CAMERA_FB_IN_PSRAM=1;
struct camera_config_t {int ledc_channel,ledc_timer,pin_pwdn,pin_reset,pin_xclk,pin_sccb_sda,pin_sccb_scl,pin_d0,pin_d1,pin_d2,pin_d3,pin_d4,pin_d5,pin_d6,pin_d7,pin_vsync,pin_href,pin_pclk,xclk_freq_hz,pixel_format,frame_size,jpeg_quality,fb_count,grab_mode,fb_location;};
camera_config_t config{};int inits=0,deinits=0,returned=0;bool allocated=false,failInit=false,failProbe=false;
bool psramFound(){return true;}
int esp_camera_init(camera_config_t* c){assert(!allocated);config=*c;++inits;if(failInit)return -1;allocated=true;return 0;}
int esp_camera_deinit(){assert(allocated);++deinits;allocated=false;return 0;}
struct camera_fb_t {int format=PIXFORMAT_JPEG;unsigned len=1000;} frame;
camera_fb_t* esp_camera_fb_get(){assert(allocated);return failProbe?nullptr:&frame;}
void esp_camera_fb_return(camera_fb_t* f){assert(f==&frame);++returned;}
struct sensor_t {int (*set_framesize)(sensor_t*,framesize_t);int (*set_quality)(sensor_t*,int);struct {int PID;} id;};
int change(sensor_t*,int){return 0;}sensor_t sensor{change,change,{0x3660}};
sensor_t* esp_camera_sensor_get(){return &sensor;}
struct Logger {template<class... Args>void printf(const char*,Args...){}} Serial;
${source}
int main(){
 using namespace ChakshuCamera;VideoProfile p;
 assert(begin()&&config.frame_size==FRAMESIZE_VGA&&config.fb_count==1);
 assert(beginVideo(0,p)&&config.frame_size==FRAMESIZE_QXGA&&config.fb_count==2&&config.grab_mode==CAMERA_GRAB_LATEST&&config.jpeg_quality==8);
 assert(!configure(true));assert(p.width==2048&&p.height==1536&&p.fps==4&&p.quality==8);
 endVideo();assert(ready&&!continuousCapture&&config.frame_size==FRAMESIZE_VGA&&config.fb_count==1&&configure(true));
 const int before=inits;assert(!beginVideo(9,p)&&inits==before);
 assert(beginVideo(1,p)&&p.width==1280&&p.height==720&&p.fps==10&&p.quality==10&&config.frame_size==FRAMESIZE_HD&&config.fb_count==2);
 failInit=true;assert(!beginVideo(0,p)&&!ready&&!allocated);failInit=false;endVideo();assert(ready&&allocated);
 failProbe=true;assert(!beginVideo(0,p)&&!ready&&!allocated);failProbe=false;endVideo();assert(ready&&config.fb_count==1);
 assert(deinits>4&&returned>4);puts("PASS SD resolution allocation and phone restoration");
}`;
 assert.match(nativeTest(fixture),/PASS SD resolution allocation/);
});
