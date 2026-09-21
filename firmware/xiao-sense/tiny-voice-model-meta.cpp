// Generated Chakshu TinyML model with user-calibrated offline commands.
// Architecture: 16 spectral bins x 30 time frames -> Conv20 -> Conv20 ->
// global max + four temporal means -> 8 classes.
// Learned weights: 2,960 bytes int8.
// Quantized synthetic held-out accuracy: 89.2%.
// Supplied-speaker aligned repetition accuracy: 82.1%.
// Raw recordings are not stored; derived calibration features live in
// tools/chakshu-voice-calibration-v1.json.
#pragma once
#include <stdint.h>
namespace ChakshuTinyModel {
constexpr uint16_t MODEL_SAMPLE_RATE=16000;
constexpr uint16_t WINDOW_SAMPLES=24000;
constexpr uint16_t FRAME_SAMPLES=512;
constexpr uint16_t FRAME_HOP=800;
constexpr uint8_t TIME_FRAMES=30;
constexpr uint8_t BANDS=16;
constexpr uint8_t CHANNELS=20;
constexpr uint8_t POOL_REGIONS=5;
constexpr uint8_t POOL_FEATURES=CHANNELS*POOL_REGIONS;
constexpr uint8_t CLASSES=8;
enum Class : uint8_t { NOISE=0, UNKNOWN=1, HEY_SNAP=2, PHOTO=3, VIDEO=4, AUDIO=5, DESCRIBE=6, STOP=7 };
constexpr float GOERTZEL_COEFF[16] = {
  1.99623621f, 1.99036944f, 1.97835302f, 1.95663476f, 1.92086101f, 1.85701215f, 1.75214016f, 1.57669282f,
  1.3063457f, 0.92107743f, 0.390180647f, -0.269161403f, -0.942793489f, -1.53033447f, -1.88308811f, -1.9975909f
};
constexpr float FEATURE_MEAN[16] = {
  0.514208496f, 0.660700381f, 0.684210718f, 0.552879632f, 0.400273651f, 0.29174912f, 0.202825919f, 0.171828747f,
  0.169519797f, 0.199321613f, 0.170960397f, 0.101196863f, 0.0993902609f, 0.09449552f, 0.0940129384f, 0.0903223678f
};
constexpr float FEATURE_INV_STD[16] = {
  1.23877597f, 0.888272107f, 0.810282767f, 0.875891685f, 1.05606234f, 1.33191299f, 1.92205513f, 2.33884835f,
  2.57671952f, 2.3032546f, 2.76177669f, 5.0012579f, 4.96253729f, 5.21072769f, 5.17992306f, 5.37818527f
};
constexpr float C1_SCALE=0.00918776076f;
