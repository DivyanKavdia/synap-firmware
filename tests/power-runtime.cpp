#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
uint32_t now=0,lastLedPattern=UINT32_MAX;
uint32_t millis(){return now;}
bool updating=false,remoteStandby=false,batteryAvailable=false;
uint16_t batteryMillivolts=3600;
constexpr uint16_t BATTERY_LOW_MV=3600;
constexpr uint8_t LED_DIM=4;
enum class DeviceState{DISCONNECTED,CONNECTED_IDLE,STREAMING,ERROR};
DeviceState deviceState=DeviceState::CONNECTED_IDLE;
bool otaBusy(){return updating;}
struct Led {
  uint32_t pattern=0;unsigned writes=0;
  uint32_t Color(uint8_t r,uint8_t g,uint8_t b){return (uint32_t(r)<<16)|(uint32_t(g)<<8)|b;}
  void setPixelColor(int,uint32_t value){pattern=value;}
  void show(){++writes;}
} statusLed;
// INSERT LED
int bleServer=1,audioFrameQueue=1,controlQueue=1;
std::atomic<bool> microphoneValidated{false};
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NOT_FOUND=1;
int validationResult=-1;
unsigned validations=0,waitMs=0;
int esp_ota_mark_app_valid_cancel_rollback(){++validations;return validationResult;}
void delay(unsigned ms){waitMs=ms;}
struct SerialPort {template<typename... T> void printf(const char*,T...) {}} Serial;
// INSERT LOOP
int main(){
  updateStatusLed(false);assert(statusLed.pattern==4);
  remoteStandby=true;updateStatusLed(false);assert(statusLed.pattern==0);
  batteryAvailable=true;deviceState=DeviceState::DISCONNECTED;
  const auto writes=statusLed.writes;
  for(now=0;now<7000;++now){updateStatusLed(false);assert(statusLed.pattern==0);}
  assert(statusLed.writes==writes);
  now=0;updating=true;updateStatusLed(false);assert(statusLed.pattern==0x040200);
  updating=false;remoteStandby=false;updateStatusLed(false);assert(statusLed.pattern==0x040000);
  batteryAvailable=false;deviceState=DeviceState::STREAMING;
  updateStatusLed(false);assert(statusLed.pattern==0x000500);

  now=5000;loop();assert(validations==0);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  assert(waitMs==20);
  now=5001;
#if USE_REAL_I2S_MIC
  loop();assert(validations==0 && waitMs==20);
#endif
  microphoneValidated=true;
  bleServer=0;loop();assert(validations==0 && waitMs==20);
  bleServer=1;loop();assert(validations==1 && waitMs==20);
  validationResult=ESP_OK;loop();assert(validations==2 && waitMs==1000);
  loop();assert(validations==2 && waitMs==1000);
#else
  assert(waitMs==1000);
#endif
  std::cout<<"PASS power: dark standby, unchanged active indications and gated boot validation\n";
}
