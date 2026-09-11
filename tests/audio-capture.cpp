#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define USE_REAL_I2S_MIC 1
constexpr uint16_t SAMPLES_PER_FRAME=800;
struct AudioFrame { uint32_t generation; uint16_t sequence; int16_t samples[SAMPLES_PER_FRAME]; };
std::atomic<bool> streamingEnabled{true};
std::atomic<uint32_t> streamGeneration{1};
int microphoneLockDepth=0;
struct MicrophoneGuard { MicrophoneGuard(){++microphoneLockDepth;} ~MicrophoneGuard(){--microphoneLockDepth;} };
struct SerialStub { void println(const char*) {} } Serial;
static void vTaskDelay(int) {}
static int pdMS_TO_TICKS(int ms) { return ms; }

struct FakeI2S {
  std::vector<uint8_t> data;
  std::vector<size_t> readSizes;
  size_t position=0,readIndex=0;
  bool cancelOnNextRead=false;
  void load(const std::vector<int16_t>& pcm) {
    data.resize(pcm.size()*4); position=0; readIndex=0;
    for(size_t i=0;i<pcm.size();++i){
      const int32_t raw=static_cast<int32_t>(int64_t(pcm[i])*65536);
      std::memcpy(data.data()+i*4,&raw,4);
    }
  }
  size_t readBytes(char* dest,size_t count) {
    assert(microphoneLockDepth==1);
    if(cancelOnNextRead){streamingEnabled.store(false);cancelOnNextRead=false;}
    if(readIndex<readSizes.size())count=std::min(count,readSizes[readIndex++]);
    count=std::min(count,data.size()-position);
    if(count)std::memcpy(dest,data.data()+position,count);
    position+=count;return count;
  }
} microphoneI2S;
std::vector<int16_t> recoveryPcm;
unsigned starts=0,stops=0;
static void stopMicrophone() { ++stops; }
static bool startMicrophone() {
  ++starts;
  microphoneI2S.load(recoveryPcm);
  microphoneI2S.readSizes.clear();
  return true;
}

// INSERT PRODUCTION ACQUIRE

static std::vector<int16_t> signal(size_t size,int phase=0) {
  std::vector<int16_t> result(size);
  for(size_t i=0;i<size;++i)result[i]=int16_t(((i+phase)*7919)%60001-30000);
  return result;
}

int main() {
  auto pcm=signal(1600);
  microphoneI2S.load(pcm);
  // Odd byte boundaries are legal Stream reads; accumulation must preserve slots.
  microphoneI2S.readSizes={1,2,7,13,127,3,31};
  AudioFrame frame{1,0,{}};
  SynapAudio::SpeechHighPass reference;
  for(int part=0;part<2;++part){
    assert(acquireAudioFrame(frame));
    for(int i=0;i<800;++i)assert(frame.samples[i]==reference.process(pcm[part*800+i]));
  }

  // A new generation is independent of the preceding recording.
  pcm=signal(800,13);
  microphoneI2S.load(pcm);microphoneI2S.readSizes.clear();
  frame.generation=2;streamGeneration.store(2);reference.reset();
  assert(acquireAudioFrame(frame));
  for(int i=0;i<800;++i)assert(frame.samples[i]==reference.process(pcm[i]));

  // Three empty reads trigger the existing bounded restart; old partial PCM and
  // filter history must both be discarded before the recovered frame is encoded.
  microphoneI2S.load(signal(800));
  microphoneI2S.readSizes={17,0,0,0};
  recoveryPcm=signal(800,99);reference.reset();
  assert(acquireAudioFrame(frame));
  assert(starts==1 && stops==1);
  for(int i=0;i<800;++i)assert(frame.samples[i]==reference.process(recoveryPcm[i]));

  // Stopping during a partial read aborts capture rather than publishing a frame.
  microphoneI2S.load(signal(800));microphoneI2S.readSizes={4};
  microphoneI2S.cancelOnNextRead=true;
  assert(!acquireAudioFrame(frame));
  assert(starts==1);
  std::puts("PASS: exact production capture preserves byte alignment, frame continuity, generation reset, recovery reset and stop cancellation");
}
