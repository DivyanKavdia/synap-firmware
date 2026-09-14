// XIAO Sense camera owns GPIO10/40/39/15/17/18/16/14/12/11/48/38/47/13.
// ESP32-S3 camera uses LCD_CAM; the onboard PDM microphone uses I2S0.
#include <esp_camera.h>
namespace ChakshuCamera {
bool ready=false;
uint16_t sensorPid=0;
bool begin() {
  if (ready) {
    camera_fb_t* probe=esp_camera_fb_get();
    const bool healthy=probe && probe->format==PIXFORMAT_JPEG && probe->len>4;
    if (probe) esp_camera_fb_return(probe);
    if (healthy) return true;
    esp_camera_deinit();ready=false;
  }
  if (!psramFound()) return false;
  camera_config_t config{};
  config.ledc_channel=LEDC_CHANNEL_0;
  config.ledc_timer=LEDC_TIMER_0;
  config.pin_pwdn=-1; config.pin_reset=-1;
  config.pin_xclk=10; config.pin_sccb_sda=40; config.pin_sccb_scl=39;
  config.pin_d0=15; config.pin_d1=17; config.pin_d2=18; config.pin_d3=16;
  config.pin_d4=14; config.pin_d5=12; config.pin_d6=11; config.pin_d7=48;
  config.pin_vsync=38; config.pin_href=47; config.pin_pclk=13;
  config.xclk_freq_hz=20000000;
  config.pixel_format=PIXFORMAT_JPEG;
  config.frame_size=FRAMESIZE_VGA;
  config.jpeg_quality=12;
  config.fb_count=1;
  config.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location=CAMERA_FB_IN_PSRAM;
  const esp_err_t error=esp_camera_init(&config);
  if (error!=ESP_OK) {
    Serial.printf("[CHAKSHU] camera init failed: 0x%x\n",unsigned(error));
    return false;
  }
  sensor_t* sensor=esp_camera_sensor_get();
  sensorPid=sensor?uint16_t(sensor->id.PID):0;
  camera_fb_t* frame=esp_camera_fb_get();
  ready=frame && frame->format==PIXFORMAT_JPEG && frame->len>4;
  if (frame) esp_camera_fb_return(frame);
  if (!ready) esp_camera_deinit();
  Serial.printf("[CHAKSHU] camera pid=0x%04x ready=%u\n",unsigned(sensorPid),unsigned(ready));
  return ready;
}
}
