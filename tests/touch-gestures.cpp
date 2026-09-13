#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
constexpr int TOUCH_INPUT_PIN=CONFIG_IDF_TARGET_ESP32C3?3:13, TOUCH_ACTIVE_LEVEL=1;
constexpr uint32_t TOUCH_DEBOUNCE_MS=35, SYNAP_DEEP_SLEEP_MARKER=123;
constexpr uint8_t CMD_START=1,CMD_STOP=2,PROTOCOL_VERSION=2;
constexpr int SLEEP_STAGE_RESET_RECOVERY=1,SLEEP_STAGE_WAKE_VALIDATING=2,SLEEP_STAGE_WAKE_CONFIRMED=3;
constexpr int ESP_SLEEP_WAKEUP_GPIO=7;
constexpr int ESP_SLEEP_WAKEUP_EXT0=2;
constexpr int TOUCH_WAKE_CAUSE=CONFIG_IDF_TARGET_ESP32C3?ESP_SLEEP_WAKEUP_GPIO:ESP_SLEEP_WAKEUP_EXT0;
constexpr int WRONG_TOUCH_WAKE_CAUSE=CONFIG_IDF_TARGET_ESP32C3?ESP_SLEEP_WAKEUP_EXT0:ESP_SLEEP_WAKEUP_GPIO;
using esp_sleep_wakeup_cause_t=int;
uint32_t clockMs=1000,touchChangedAt=0,touchPressedAt=0;
bool input=false,touchRawState=false,touchStableState=false;
bool remoteStandby=false,sleepPending=false,busy=false;
std::atomic<bool> deviceConnected{true},streamingEnabled{false};
std::atomic<uint32_t> connectionGeneration{1};
bool durableLock=false,bootSleepWasLocked=false,clearSucceeds=true;
uint32_t synapDeepSleepMarker=0,synapSleepRequestCounter=0;
int synapLastSleepStage=0,bootWakeCause=0,wakeCause=TOUCH_WAKE_CAUSE;
int sleeps=0,starts=0,stops=0,clears=0;
bool timedInput=false;
uint32_t wakeStart=0,releaseAfter=0;
struct Logger { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
uint32_t millis(){return clockMs;}
int digitalRead(int pin){assert(pin==TOUCH_INPUT_PIN);return timedInput ? uint32_t(clockMs-wakeStart)<releaseAfter : input;}
void delay(uint32_t ms){clockMs+=ms;}
bool otaBusy(){return busy;}
bool readDurableSleepLock(){return durableLock;}
bool writeDurableSleepLock(bool value){
  if(!clearSucceeds)return false;
  durableLock=value;
  if(!value){assert(!digitalRead(TOUCH_INPUT_PIN));++clears;}
  return true;
}
int esp_sleep_get_wakeup_cause(){return wakeCause;}
void armTouchWakeAndSleep(){++sleeps;}
void stopStreaming(){++stops;streamingEnabled=false;}
void enterDeepSleep(const char*){assert(!input && !streamingEnabled && !busy);++sleeps;}
void enterRemoteStandby(){assert(!streamingEnabled);remoteStandby=true;}
void processCommand(uint8_t cmd,uint8_t){
  assert(!busy);
  if(cmd==CMD_STOP)stopStreaming();
  else {assert(deviceConnected);++starts;remoteStandby=false;streamingEnabled=true;}
}
std::vector<uint8_t> commands;
enum class EventType { COMMAND };
std::atomic<uint32_t> streamGeneration{1};
void queueEvent(EventType,uint8_t cmd,uint8_t,uint32_t){commands.push_back(cmd);}
void drain(){for(auto cmd:commands)processCommand(cmd,PROTOCOL_VERSION);commands.clear();}
// INSERT WAKE
// INSERT POLL
void advance(uint32_t duration,bool level,bool consumeCommands=true){
  input=level;
  for(uint32_t i=0;i<duration;i+=5){pollTouchControl();if(consumeCommands)drain();clockMs+=5;}
  pollTouchControl();
}
void tap(){advance(120,true);advance(120,false);}
void settle(){advance(1000,false);}
void checkWake(uint32_t duration,bool expected,bool lock=true,int cause=TOUCH_WAKE_CAUSE){
  durableLock=lock;bootSleepWasLocked=false;synapDeepSleepMarker=0;sleepPending=false;
  wakeCause=cause;timedInput=true;wakeStart=clockMs;releaseAfter=duration;
  const int beforeSleeps=sleeps,beforeClears=clears,beforeStarts=starts;
  const bool result=confirmTouchWakeGesture();
  assert(result==expected);
  if(!expected){assert(sleeps==beforeSleeps+1);assert(durableLock);assert(clears==beforeClears);}
  else if(lock){assert(!durableLock && !sleepPending);assert(clears==beforeClears+1);}
  assert(starts==beforeStarts);
  timedInput=false;input=false;
}
int main(){
  settle();tap();assert(starts==0);tap();assert(starts==1 && streamingEnabled);
  // A third tap cannot sleep either board or undo the completed double tap.
  tap();assert(sleeps==0 && stops==0);settle();
  tap();tap();assert(stops==1 && remoteStandby);settle();
  tap();tap();assert(starts==2 && !remoteStandby);settle();
  advance(3995,true);advance(100,false);assert(sleeps==0 && stops==1);settle();
  advance(4000,true);assert(sleeps==0 && streamingEnabled);
  advance(100,false);assert(sleeps==1 && stops==2);settle();
  deviceConnected=false;++connectionGeneration;
  tap();tap();assert(starts==2);settle();
  advance(4100,true);advance(100,false);assert(sleeps==2);settle();
  // Holds interrupted by OTA are discarded, even if OTA finishes before release.
  advance(2000,true);busy=true;advance(100,true);busy=false;
  advance(3000,true);advance(100,false);assert(sleeps==2);settle();
  // A reconnect cannot join a tap from the previous connection.
  deviceConnected=true;++connectionGeneration;settle();tap();
  deviceConnected=false;advance(5,false);deviceConnected=true;advance(5,false);tap();assert(starts==2);settle();
  advance(20,true);advance(100,false);tap();assert(starts==2);settle();
  // Elapsed-time checks remain valid across the millis wrap.
  clockMs=0xfffff800u;deviceConnected=!deviceConnected;settle();advance(4000,true);advance(100,false);assert(sleeps==3);
  // Sleep must wait for the queued STOP to finish, not just for touch release.
  deviceConnected=true;streamingEnabled=true;settle();
  const int beforeDeferredSleep=sleeps,beforeDeferredStop=stops;
  advance(4100,true,false);advance(100,false,false);
  assert(sleeps==beforeDeferredSleep && streamingEnabled && commands.size()==1);
  assert(commands.front()==CMD_STOP);
  drain();advance(5,false);
  assert(sleeps==beforeDeferredSleep+1 && stops==beforeDeferredStop+1 && !streamingEnabled);
  settle();
  checkWake(0,true,false); // Normal cold boot does not require a hold.
  checkWake(120,false);checkWake(3995,false);checkWake(4000,true);checkWake(6500,true);
  clearSucceeds=false;checkWake(4100,false);clearSucceeds=true;
  checkWake(5000,false,true,0); // Reset with a durable sleep lock cannot boot BLE.
  checkWake(5000,false,true,WRONG_TOUCH_WAKE_CAUSE);
  clockMs=0xffffff00u;checkWake(4100,true);
  std::puts("PASS shared touch: double tap, hold/release, delayed STOP, OTA, reconnect, wrap and wake lock");
}
