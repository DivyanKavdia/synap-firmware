// Retained evidence from the last link survives reconnect and recording starts.
// GAP callbacks only write atomics; diagnostic reads perform no radio requests.
namespace ChakshuLink {
std::atomic<uint32_t> bootReadyMs{0},mediaBootMs{0},connectedAt{0},lastDurationMs{0};
std::atomic<uint16_t> lastInterval{0},lastLatency{0},lastTimeout{0};
std::atomic<uint16_t> interval{0},latency{0},timeout{0};
std::atomic<uint16_t> paramRequestCode{0xFFFF},lastParamRequestCode{0xFFFF};
std::atomic<uint8_t> paramRequests{0},lastParamRequests{0};
std::atomic<uint8_t> lastStage{0};
std::atomic<bool> statusSeen{false};
uint8_t stage(bool connected,bool streaming,bool subscribed) {
  return !connected?0:streaming?4:statusSeen.load()?3:subscribed?2:1;
}
void append(uint8_t* value,bool connected,bool streaming,bool subscribed,uint32_t now) {
  auto u32=[&](unsigned offset,uint32_t n) {
    for(unsigned i=0;i<4;++i)value[offset+i]=uint8_t(n>>(8*i));
  };
  auto u16=[&](unsigned offset,uint16_t n) {value[offset]=uint8_t(n);value[offset+1]=uint8_t(n>>8);};
  u32(48,bootReadyMs.load());u32(52,mediaBootMs.load());u32(56,lastDurationMs.load());
  u16(60,lastInterval.load());u16(62,lastLatency.load());u16(64,lastTimeout.load());
  value[66]=lastStage.load();value[67]=stage(connected,streaming,subscribed);
  u32(68,connected?uint32_t(now-connectedAt.load()):0);
  u16(72,connected?interval.load():0);u16(74,connected?latency.load():0);
  u16(76,connected?timeout.load():0);
  value[78]=paramRequests.load();value[79]=lastParamRequests.load();
  u16(80,paramRequestCode.load());u16(82,lastParamRequestCode.load());
}
}
