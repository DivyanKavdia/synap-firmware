'use strict';
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');

function apply(source){
  let out=source;
  out=replaceOnce(out,'#include <Adafruit_NeoPixel.h>','#include <driver/gpio.h>', 'C3 has a discrete LED');
  out=replaceOnce(out,'Adafruit_NeoPixel statusLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);','', 'C3 removes NeoPixel driver');
  out=replaceFunctionBlock(out,'void updateStatusLed(bool force) {','void setDeviceState(',readTemplate('esp32c3','status-led.cpp')+'\n', 'C3 onboard LED patterns');
  out=replaceOnce(out,`  statusLed.begin();
  statusLed.clear();
  statusLed.show();`, `  gpio_set_level(static_cast<gpio_num_t>(RGB_LED_PIN),HIGH);
  pinMode(RGB_LED_PIN,OUTPUT);
  gpio_hold_dis(static_cast<gpio_num_t>(RGB_LED_PIN));`, 'C3 LED initialization');
  out=out.split('statusLed.clear();statusLed.show();').join('digitalWrite(RGB_LED_PIN,HIGH);lastLedPattern=0;');
  // Hold the inactive level through both normal and fail-closed deep-sleep paths.
  out=out.split('  esp_deep_sleep_start();').join(`  digitalWrite(RGB_LED_PIN,HIGH);
  gpio_hold_en(static_cast<gpio_num_t>(RGB_LED_PIN));
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();`);

  return out;
}

module.exports={apply};
