'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('camera mode changes preserve VGA photos and reject failed sensor changes without leaking a frame',()=>{
 const camera=fs.readFileSync('firmware/xiao-sense/camera.cpp','utf8');
 const config=camera.slice(camera.indexOf('framesize_t frameSize='),camera.indexOf('bool begin()'));
 const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
 const capture=transfer.slice(transfer.indexOf('uint8_t captureFrame('),transfer.indexOf('struct PreviewJpeg'));
 const fixture=`#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
using framesize_t=int;constexpr int FRAMESIZE_VGA=2,FRAMESIZE_QVGA=1,PIXFORMAT_JPEG=7;
int changes=0,mode=FRAMESIZE_VGA,returns=0,gets=0;bool sensorPresent=true,changeFails=false,allocateFails=false,getFails=false;
struct sensor_t {int (*set_framesize)(sensor_t*,framesize_t);int (*set_quality)(sensor_t*,int);};
int change(sensor_t*,framesize_t next){++changes;if(changeFails)return -1;mode=next;return 0;}
int quality=12;int setQuality(sensor_t*,int q){quality=q;return 0;}
sensor_t sensor{change,setQuality};sensor_t* esp_camera_sensor_get(){return sensorPresent?&sensor:nullptr;}
namespace ChakshuCamera {bool ready=true;${config}}
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
