// WakeNet-gated MultiNet command IDs and activation gate shared by firmware and native tests.
namespace ChakshuVoice {
enum Command : uint8_t {
  WAKE=1, PHOTO=2, VIDEO_START=3, VIDEO_STOP=4, AUDIO_ON=5, AUDIO_OFF=6, DESCRIBE=7,
  DURATION_BASE=20
};
constexpr uint8_t DURATION_LAST=155; // 1..120 seconds + selected 30-second steps to 600.
inline bool durationCommand(uint8_t command){return command>=DURATION_BASE&&command<=DURATION_LAST;}
inline uint16_t durationSeconds(uint8_t command) {
  if(command<DURATION_BASE)return 0;
  const uint16_t index=uint16_t(command-DURATION_BASE);
  if(index<120)return index+1;
  static constexpr uint16_t tail[]={150,180,210,240,270,300,330,360,390,420,450,480,510,540,570,600};
  return index<120+sizeof(tail)/sizeof(tail[0])?tail[index-120]:0;
}
inline void put16le(uint8_t* p,uint16_t value){p[0]=uint8_t(value);p[1]=uint8_t(value>>8);}
class Gate {
  bool armed=false;uint32_t armedAt=0,lastAction=0;bool acted=false;
public:
  void reset(){armed=false;}
  uint8_t accept(uint8_t command,float confidence,uint32_t now) {
    if(confidence<0.90f||confidence>1.0f||!(confidence>=0.90f))return 0;
    if(command==WAKE){armed=true;armedAt=now;return WAKE;}
    const bool recognized=(command>=PHOTO&&command<=DESCRIBE)||durationCommand(command);
    const bool allowed=armed&&uint32_t(now-armedAt)<=8000u&&recognized;
    armed=false;
    if(!allowed||(acted&&uint32_t(now-lastAction)<1200u))return 0;
    acted=true;lastAction=now;return command;
  }
};
void feed(const int16_t* samples,size_t count);
bool active();
void initialize();
void tick();
void ble(BLEService* service);
}