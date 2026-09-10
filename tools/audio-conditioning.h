#ifndef SYNAP_AUDIO_CONDITIONING_H
#define SYNAP_AUDIO_CONDITIONING_H

#include <stdint.h>

// Set to 0 in a comparison build to retain the original unity-gain PCM path.
#ifndef SYNAP_MIC_HPF_ENABLE
#define SYNAP_MIC_HPF_ENABLE 1
#endif

namespace SynapAudio {

// First-order 70 Hz high-pass at 16 kHz, with unity Nyquist gain.
// a = exp(-2*pi*70/16000), b = (1+a)/2, quantized to Q15.
// Q8 state avoids integer limit-cycle noise on quiet signals. Products use
// int64_t so full-scale steps cannot overflow, including on the ESP32-C3.
// This removes rumble/DC, not speech-band noise. No gain, gate or VAD is used.
class SpeechHighPass {
 public:
  void reset() { previousInputQ8_=0; previousOutputQ8_=0; initialized_=false; }

  int16_t process(int16_t sample) {
#if SYNAP_MIC_HPF_ENABLE
    const int32_t inputQ8=int32_t(sample)*256;
    if (!initialized_) {
      previousInputQ8_=inputQ8;
      initialized_=true;
      return 0;
    }
    const int64_t nextQ8=(int64_t(previousOutputQ8_)*31880 +
      (int64_t(inputQ8)-previousInputQ8_)*32324)/32768;
    previousInputQ8_=inputQ8;
    previousOutputQ8_=static_cast<int32_t>(nextQ8);
    // Symmetric rounding avoids adding a negative DC bias to quiet audio.
    int32_t output=static_cast<int32_t>((nextQ8+(nextQ8<0 ? -128 : 128))/256);
    if (output>32767) output=32767;
    if (output<-32768) output=-32768;
    return static_cast<int16_t>(output);
#else
    return sample;
#endif
  }

 private:
  int32_t previousInputQ8_=0, previousOutputQ8_=0;
  bool initialized_=false;
};

} // namespace SynapAudio

#endif
