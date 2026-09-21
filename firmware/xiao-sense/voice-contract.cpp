// TinyML command IDs and wake gate shared by firmware and native tests.
namespace ChakshuVoice {
enum Command : uint8_t {
  WAKE=1, PHOTO=2, VIDEO_START=3, VIDEO_STOP=4, AUDIO_ON=5, AUDIO_OFF=6, DESCRIBE=7, STOP=8
};
inline void put16le(uint8_t* p,uint16_t value){p[0]=uint8_t(value);p[1]=uint8_t(value>>8);}
class Gate {
  bool armed=false;uint32_t armedAt=0,lastAction=0;bool acted=false;
  static constexpr uint32_t MIN_ACTION_DELAY_MS=900u;
public:
  void reset(){armed=false;}
  uint8_t accept(uint8_t command,float confidence,uint32_t now) {
    if(confidence>1.0f||!(confidence>=0.0f))return 0;
    if(command==WAKE){
      if(confidence<0.70f)return 0;
      armed=true;armedAt=now;return WAKE;
    }
    if(confidence<0.74f)return 0;
    const bool recognized=command>=PHOTO&&command<=STOP;
    const uint32_t age=uint32_t(now-armedAt);
    // Do not let the overlapping inference windows from the wake utterance
    // become a media action. A fresh command must follow the wake phrase.
    const bool allowed=armed&&age>=MIN_ACTION_DELAY_MS&&age<=5000u&&recognized;
    if(!allowed)return 0;
    armed=false;
    if(acted&&uint32_t(now-lastAction)<1500u)return 0;
    acted=true;lastAction=now;return command;
  }
};
void feed(const int16_t* samples,size_t count);
bool active();
void initialize();
void scheduleInitialize();
void tick();
void mediaCompleted(uint8_t operation,uint8_t error);
void linkConnected();
void linkDisconnected();
void ble(BLEService* service);
}
