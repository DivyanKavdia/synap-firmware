#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
constexpr uint32_t ACTIVE_CPU_MHZ=160,IDLE_CPU_MHZ=80;
std::atomic<bool> deviceConnected{false},odysseyRecording{false};
std::atomic<uint32_t> lastDisconnectAt{0};
bool wifiBusy=false,sdTransferBusy=false;
namespace OdysseyWifi {bool busy(){return wifiBusy;}}
namespace OdysseyTransfer {bool busy(){return sdTransferBusy;}}
uint32_t now=100000,frequency=0,changes=0;
uint32_t millis(){return now;}
bool setCpuFrequencyMhz(uint32_t mhz){frequency=mhz;++changes;return true;}
struct Logger {template<class... T> void printf(const char*,T...) {}} Serial;
// INSERT CPU
int main(){
  applyCpuPowerProfile(false);
  assert(frequency==80 && changes==1);
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  deviceConnected=true;
  applyCpuPowerProfile(false);
  assert(frequency==160 && changes==2);
  now+=160000;
  applyCpuPowerProfile(false);
  assert(frequency==160 && changes==2);
  deviceConnected=false;
  lastDisconnectAt=now;
  applyCpuPowerProfile(false);
  assert(frequency==160 && changes==2);
  now+=11999;
  applyCpuPowerProfile(false);
  assert(frequency==160);
  now++;
  applyCpuPowerProfile(false);
  assert(frequency==80);
  sdTransferBusy=true;
  applyCpuPowerProfile(false);
  assert(frequency==160);
  sdTransferBusy=false;
  applyCpuPowerProfile(false);
  assert(frequency==80);
#else
  deviceConnected=true;
  applyCpuPowerProfile(false);
  assert(frequency==80 && changes==1);
  applyCpuPowerProfile(true);
  assert(frequency==160);
#endif
  puts("PASS C3 BLE connection keeps active CPU and bounds post-disconnect boost");
}
