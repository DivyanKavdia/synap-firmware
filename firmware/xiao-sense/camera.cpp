// XIAO Sense camera owns GPIO10/40/39/15/17/18/16/14/12/11/48/38/47/13.
// ESP32-S3 camera uses LCD_CAM; the onboard PDM microphone uses I2S0.
#include <esp_camera.h>
namespace ChakshuCamera {
bool ready=false;
uint16_t sensorPid=0;
framesize_t frameSize=FRAMESIZE_VGA;
int jpegQuality=12;
bool continuousCapture=false;
struct VideoProfile { framesize_t size;uint16_t width,height;uint8_t fps,quality; };

bool ov3660(){return sensorPid==0x3660;}

// Profile 0 is the highest practical native-resolution SD profile. OV3660 can
// expose QXGA 2048x1536; the common OV2640 Sense module falls back to UXGA.
// Profile 1 trades detail for frame rate without dropping to phone-preview VGA.
bool videoProfile(uint32_t id,VideoProfile& profile) {
  if(id==0){
    profile=ov3660()?VideoProfile{FRAMESIZE_QXGA,2048,1536,4,8}
                    :VideoProfile{FRAMESIZE_UXGA,1600,1200,5,10};
    return true;
  }
  if(id==1){profile={FRAMESIZE_HD,1280,720,10,10};return true;}
  return false;
}

bool configure(bool preview) {
  if (!ready || continuousCapture) return false;
  const framesize_t next=preview?FRAMESIZE_QVGA:FRAMESIZE_VGA;
  const int quality=preview?22:12;
  if (next==frameSize && quality==jpegQuality) return true;
  sensor_t* sensor=esp_camera_sensor_get();
  if (!sensor) return false;
  if(next!=frameSize) {
    if(sensor->set_framesize(sensor,next)!=0)return false;
    frameSize=next;
  }
  if(quality!=jpegQuality) {
    if(sensor->set_quality(sensor,quality)!=0)return false;
    jpegQuality=quality;
  }
  return true;
}

// Allocate at the recording resolution. Changing only the sensor size leaves
// VGA-sized buffers behind and can truncate a larger JPEG. Continuous capture
// is reserved for SD: phone preview retains its inexpensive single buffer.
bool initialize(framesize_t size,bool continuous,int quality=12) {
  if(ready)esp_camera_deinit();
  ready=false;continuousCapture=false;
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
  config.frame_size=size;
  config.jpeg_quality=quality;
  config.fb_count=continuous?2:1;
  config.grab_mode=continuous?CAMERA_GRAB_LATEST:CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location=CAMERA_FB_IN_PSRAM;
  const esp_err_t error=esp_camera_init(&config);
  if (error!=ESP_OK) {
    Serial.printf("[CHAKSHU] camera init failed: 0x%x\n",unsigned(error));
    return false;
  }
  frameSize=size;jpegQuality=quality;continuousCapture=continuous;
  sensor_t* sensor=esp_camera_sensor_get();
  sensorPid=sensor?uint16_t(sensor->id.PID):0;
  camera_fb_t* frame=esp_camera_fb_get();
  ready=frame && frame->format==PIXFORMAT_JPEG && frame->len>4;
  if (frame) esp_camera_fb_return(frame);
  if (!ready) esp_camera_deinit();
  Serial.printf("[CHAKSHU] camera pid=0x%04x ready=%u\n",unsigned(sensorPid),unsigned(ready));
  return ready;
}

bool begin() {
  if(ready && !continuousCapture && frameSize==FRAMESIZE_VGA) {
    camera_fb_t* probe=esp_camera_fb_get();
    const bool healthy=probe && probe->format==PIXFORMAT_JPEG && probe->len>4;
    if(probe)esp_camera_fb_return(probe);
    if(healthy)return true;
  }
  return initialize(FRAMESIZE_VGA,false,12);
}

// Full-resolution stills need their framebuffer allocated at the requested
// sensor size. This is used only for explicit original/photo operations and is
// restored to the lightweight VGA phone path immediately afterward.
bool beginOriginal() {
  const framesize_t size=ov3660()?FRAMESIZE_QXGA:FRAMESIZE_UXGA;
  const int quality=ov3660()?8:10;
  return initialize(size,false,quality);
}
void endOriginal(){initialize(FRAMESIZE_VGA,false,12);}

bool beginVideo(uint32_t id,VideoProfile& profile) {
  return videoProfile(id,profile) && initialize(profile.size,true,profile.quality);
}
void endVideo() { initialize(FRAMESIZE_VGA,false,12); }
}