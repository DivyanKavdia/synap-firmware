#include <assert.h>
#include <cmath>
#include <cstdio>
#include <climits>
#include <chrono>
#include <vector>

constexpr double PI_AUDIO=3.14159265358979323846;

[[maybe_unused]] static double gainAt(double frequency, int amplitude=10000) {
  SynapAudio::SpeechHighPass filter;
  double inputEnergy=0,outputEnergy=0;
  for (int i=0;i<32000;++i) {
    const int16_t input=int16_t(std::lround(amplitude*std::sin(2*PI_AUDIO*frequency*i/16000)));
    const int16_t output=filter.process(input);
    if (i>=16000) { inputEnergy+=double(input)*input; outputEnergy+=double(output)*output; }
  }
  return 20*std::log10(std::sqrt(outputEnergy/inputEnergy));
}

int main() {
#if !SYNAP_MIC_HPF_ENABLE
  SynapAudio::SpeechHighPass bypass;
  for (int sample=INT16_MIN;sample<=INT16_MAX;++sample)assert(bypass.process(int16_t(sample))==sample);
  std::puts("PASS: bypass preserves every signed PCM16 value");
  return 0;
#else
  static_assert(sizeof(SynapAudio::SpeechHighPass)<=16, "Audio filter must remain tiny");
  for (int level : {INT16_MIN,-10000,-1,0,1,10000,INT16_MAX}) {
    SynapAudio::SpeechHighPass filter;
    for (int i=0;i<16000;++i)assert(filter.process(int16_t(level))==0);
    for (int i=0;i<16000;++i)filter.process(int16_t(-level/2));
    for (int i=0;i<1600;++i)assert(filter.process(int16_t(-level/2))==0);
  }
  const double hz20=gainAt(20),hz50=gainAt(50),hz70=gainAt(70),hz100=gainAt(100);
  const double hz300=gainAt(300),hz1000=gainAt(1000),hz6000=gainAt(6000);
  assert(hz20<-10 && hz20>-13);
  assert(hz50<-4 && hz50>-6);
  assert(hz70<-2.9 && hz70>-3.1);
  assert(hz100<-1.6 && hz100>-1.9);
  assert(hz300>-0.25 && hz300<0.01);
  assert(hz1000>-0.04 && hz1000<0.01);
  assert(hz6000>-0.01 && hz6000<0.01);
  // Quiet speech-band tones remain present; there is no amplitude gate.
  assert(gainAt(1000,8)>-0.7);

  SynapAudio::SpeechHighPass filter;
  // A polarity reversal after sustained full-scale input must saturate, never wrap.
  assert(filter.process(INT16_MIN)==0);
  assert(filter.process(INT16_MAX)==INT16_MAX);
  filter.reset();
  assert(filter.process(INT16_MAX)==0);
  assert(filter.process(INT16_MIN)==INT16_MIN);

  uint32_t random=0x5eed1234;
  std::vector<int16_t> samples;
  for (int i=0;i<32000;++i) {
    random=random*1664525u+1013904223u;
    samples.push_back(int16_t(random>>16));
  }
  // Exercise abrupt full-range transients with UB sanitizer enabled, and prove
  // the previous recording cannot influence the next one after reset.
  for(const int16_t sample:samples)filter.process(sample);
  filter.reset();
  SynapAudio::SpeechHighPass fresh;
  for(const int16_t sample:samples)assert(filter.process(sample)==fresh.process(sample));
  const auto start=std::chrono::steady_clock::now();
  volatile int32_t checksum=0;
  for (int pass=0;pass<100;++pass)for(const int16_t sample:samples)checksum+=filter.process(sample)/32768;
  const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  std::printf("PASS: filter bytes=%zu; dB at 20/50/70/100/300/1000/6000 Hz: %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
    sizeof(filter),hz20,hz50,hz70,hz100,hz300,hz1000,hz6000);
  std::printf("Host-only throughput: 3.2M samples in %.2f ms (checksum=%d); MCU timing requires hardware.\n",elapsed,int(checksum));
  return 0;
#endif
}
