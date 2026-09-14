// MultiNet command IDs and an activation gate shared by hardware and native tests.
namespace ChakshuVoice {
enum Command : uint8_t { WAKE=1,PHOTO=2,VIDEO_START=3,VIDEO_STOP=4,AUDIO_ON=5,AUDIO_OFF=6 };
class Gate {
  bool armed=false;uint32_t armedAt=0,lastAction=0;bool acted=false;
public:
  void reset(){armed=false;}
  uint8_t accept(uint8_t command,float confidence,uint32_t now) {
    if(confidence<0.90f||confidence>1.0f||!(confidence>=0.90f))return 0;
    if(command==WAKE){armed=true;armedAt=now;return 0;}
    const bool allowed=armed&&uint32_t(now-armedAt)<=8000u&&command>=PHOTO&&command<=AUDIO_OFF;
    armed=false;
    if(!allowed||(acted&&uint32_t(now-lastAction)<1200u))return 0;
    acted=true;lastAction=now;return command-1;
  }
};
void feed(const int16_t* samples,size_t count);
bool active();
void initialize();
void tick();
void ble(BLEService* service);
}
