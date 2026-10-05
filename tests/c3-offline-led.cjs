'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('offline preparation, recording, stop and fault produce distinct actual LED colors',()=>{
 const body=`
#include <atomic>
#include <cstdint>
#include <cassert>
constexpr uint8_t LED_DIM=4;
uint32_t now=1000,lastLedPattern=0;
uint32_t millis(){return now;}
bool otaBusy(){return false;}
std::atomic<bool> odysseySdRecoveryActive{false},odysseyCaptureActive{false},odysseyStopRequested{false};
std::atomic<uint32_t> odysseyRecordingStartedAt{1000},odysseyRecordFaultAt{0},connectedLedAt{0};
bool remoteStandby=false,batteryAvailable=false;
int batteryMillivolts=4000;
constexpr int BATTERY_LOW_MV=3300;
enum class DeviceState {DISCONNECTED,CONNECTED_IDLE,STREAMING};
DeviceState deviceState=DeviceState::DISCONNECTED;
struct Led {
 uint32_t color=0;
 uint32_t Color(uint8_t r,uint8_t g,uint8_t b){return (uint32_t(r)<<16)|(uint32_t(g)<<8)|b;}
 void setPixelColor(int,uint32_t value){color=value;}
 void show(){}
} statusLed;
${fs.readFileSync('firmware/shared/status-led.cpp','utf8')}
int main(){
 odysseySdRecoveryActive=true;now=900;updateStatusLed(true);
 assert((statusLed.color&0xff)==0 && (statusLed.color&0xff00)!=0); // amber
 odysseySdRecoveryActive=false;odysseyCaptureActive=true;now=1000;updateStatusLed(true);
 assert((statusLed.color&0xff0000)!=0 && (statusLed.color&0xff)!=0 && (statusLed.color&0xff00)==0);
 now=1300;updateStatusLed(true);assert(statusLed.color==0); // purple pulse ends
 now=2800;updateStatusLed(true);assert(statusLed.color!=0); // next purple pulse
 odysseyStopRequested=true;updateStatusLed(true);assert(statusLed.color==0);
 odysseyCaptureActive=false;odysseyRecordFaultAt=now;updateStatusLed(true);
 assert((statusLed.color&0xff0000)!=0 && (statusLed.color&0xffff)==0); // red failure
}
`;
 nativeTest(body,['-DCONFIG_IDF_TARGET_ESP32C3=1','-DSYNAP_CHAKSHU=0']);
});
