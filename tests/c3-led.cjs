'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
test('C3 active-low LED duty cycles preserve recording and dark sleep',()=>{
  const source=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
  const code=materialize(source,'esp32c3-supermini-4m');
  assert.doesNotMatch(code,/statusLed\.|Adafruit_NeoPixel/);
  assert.match(code,/gpio_hold_en\(static_cast<gpio_num_t>\(RGB_LED_PIN\)\)/);
  assert.match(code,/gpio_hold_dis\(static_cast<gpio_num_t>\(RGB_LED_PIN\)\)/);
  const start=code.indexOf('void updateStatusLed(bool force) {');
  const led=code.slice(start,code.indexOf('void setDeviceState(',start));
  assert.match(nativeTest(`
#include <cstdint>
#include <cassert>
#include <cstdio>
constexpr int LOW=0,HIGH=1,RGB_LED_PIN=8;
enum class DeviceState {DISCONNECTED,CONNECTED_IDLE,STREAMING,ERROR};
DeviceState deviceState=DeviceState::DISCONNECTED;
uint32_t now=0,lastLedPattern=99;
bool remoteStandby=false,sleepPending=false,updating=false;
int level=HIGH,writes=0;
uint32_t millis(){return now;}
bool otaBusy(){return updating;}
void digitalWrite(int pin,int value){assert(pin==8);level=value;++writes;}
${led}
int onTime(int period){int count=0;for(now=0;now<uint32_t(period);++now){updateStatusLed(false);if(level==LOW)++count;}return count;}
int main(){
 assert(onTime(6000)==100);
 deviceState=DeviceState::CONNECTED_IDLE;assert(onTime(3000)==160);
 now=79;updateStatusLed(false);assert(level==LOW);
 now=80;updateStatusLed(false);assert(level==HIGH);
 now=240;updateStatusLed(false);assert(level==LOW);
 now=320;updateStatusLed(false);assert(level==HIGH);
 remoteStandby=true;assert(onTime(3000)==160);
 deviceState=DeviceState::DISCONNECTED;assert(onTime(6000)==100);
 remoteStandby=false;
 deviceState=DeviceState::STREAMING;assert(onTime(1000)==100);
 remoteStandby=false;sleepPending=true;assert(onTime(3000)==0);
 sleepPending=false;updating=true;assert(onTime(1400)==110);
 updating=false;sleepPending=true;updateStatusLed(false);const int before=writes;
 assert(onTime(8000)==0 && writes==before);
 std::puts("PASS C3 LED");
}`),/PASS C3 LED/);
});
