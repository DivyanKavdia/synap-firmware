'use strict';
const fs=require('node:fs'),path=require('node:path');
const {PRIMARY_TARGET,getTarget}=require('./targets.cjs');

function replaceOnce(source,before,after,label){
  const first=source.indexOf(before);
  if(first<0)throw Error(`Missing target materialization anchor: ${label}`);
  if(source.indexOf(before,first+before.length)>=0)throw Error(`Ambiguous target materialization anchor: ${label}`);
  return source.slice(0,first)+after+source.slice(first+before.length);
}

function replaceFunctionBlock(source,startMarker,endMarker,replacement,label){
  const start=source.indexOf(startMarker);
  if(start<0)throw Error(`Missing target materialization function: ${label}`);
  const end=source.indexOf(endMarker,start);
  if(end<0)throw Error(`Missing target materialization function end: ${label}`);
  if(source.indexOf(startMarker,start+startMarker.length)>=0)throw Error(`Ambiguous target materialization function: ${label}`);
  return source.slice(0,start)+replacement+source.slice(end);
}

function materialize(source,targetId){
  const target=getTarget(targetId);
  if(target.id===PRIMARY_TARGET)return source;
  if(target.family!=='esp32c3')throw Error(`No materializer for ${target.id}`);

  // C3 release contract: preserve S3 source byte-for-byte and apply only target-specific substitutions below.
  let out=source;
  out=out.replace(/ESP32-S3FH4R2/g,'ESP32-C3 SuperMini');
  out=out.split(PRIMARY_TARGET).join(target.id);
  out=out.split('SYNAP-ESP32S3-OTA-ID-V3').join(target.productMarker);
  out=replaceOnce(out,'p[21]!=9 || p[22]!=0','p[21]!=5 || p[22]!=0','ESP image chip ID');
  out=replaceOnce(out,'constexpr uint8_t RGB_LED_PIN = 48;','constexpr uint8_t RGB_LED_PIN = 8;','C3 status LED pin');
  out=replaceOnce(out,'#define SYNAP_TOUCH_PIN 13','#define SYNAP_TOUCH_PIN 3','C3 touch/wake pin');
  out=replaceOnce(out,'#define SYNAP_BATTERY_ADC_PIN 8','#define SYNAP_BATTERY_ADC_PIN 1','C3 battery ADC pin');
  out=out.replace(/GPIO8/g,'GPIO1');

  const c3Wake=`bool confirmTouchWakeTripleTap() {
  const bool durableLock=readDurableSleepLock();
  const bool sleepResume=durableLock || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) || bootSleepWasLocked;
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bootWakeCause=cause;
  bootSleepWasLocked=sleepResume;
  Serial.printf("[POWER] wake cause=%u sleepLock=%u rtcMarker=%u stage=%u request=%u\\n",
    unsigned(cause),durableLock?1u:0u,
    synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER?1u:0u,
    unsigned(synapLastSleepStage),unsigned(synapSleepRequestCounter));
  if (!sleepResume) return true;

  if (cause!=ESP_SLEEP_WAKEUP_GPIO) {
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] C3 sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // C3-only interaction: deliberate long press wakes the pendant. The TTP223
  // high level performs the hardware wake; firmware then confirms the hold.
  constexpr uint16_t C3_WAKE_HOLD_MS = 4000;
  synapLastSleepStage=SLEEP_STAGE_WAKE_VALIDATING;
  const uint32_t pressedAt=millis();
  Serial.println("[TOUCH] C3 wake touch detected; hold to power on");
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL &&
      uint32_t(millis()-pressedAt)<C3_WAKE_HOLD_MS) delay(5);

  if (uint32_t(millis()-pressedAt)<C3_WAKE_HOLD_MS) {
    Serial.println("[TOUCH] C3 wake press too short; returning to deep sleep");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // Do not continue into normal touch handling until the wake press is released.
  uint32_t releasedAt=millis();
  while (uint32_t(millis()-releasedAt)<TOUCH_DEBOUNCE_MS) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) releasedAt=millis();
    delay(5);
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();return false;
  }
  synapDeepSleepMarker=0;
  sleepPending=false;
  synapLastSleepStage=SLEEP_STAGE_WAKE_CONFIRMED;
  Serial.println("[TOUCH] C3 long-press wake confirmed; sleep lock cleared; continuing normal boot");
  touchRawState=false;touchStableState=false;touchPressedAt=0;
  touchChangedAt=millis();
  return true;
}

`;
  out=replaceFunctionBlock(out,'bool confirmTouchWakeTripleTap() {','void publishPowerEvent',c3Wake,'C3 wake gesture');

  const c3Touch=`void pollTouchControl() {
  constexpr uint16_t C3_TAP_MIN_MS = 60;
  constexpr uint16_t C3_TAP_MAX_MS = 500;
  constexpr uint16_t C3_DOUBLE_TAP_GAP_MS = 550;
  constexpr uint16_t C3_SLEEP_HOLD_MS = 4000;
  constexpr uint16_t C3_STATE_LOCKOUT_MS = 250;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool standbyAfterStop = false;
  static bool deepSleepAfterStop = false;
  static uint8_t tapCount = 0;
  static uint32_t lastTapAt = 0;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  // An OTA-interrupted press must not become a power gesture when OTA finishes.
  if (otaBusy() || sleepPending) {
    touchPressedAt=0;tapCount=0;lastTapAt=0;
    deepSleepAfterStop=false;standbyAfterStop=false;
  }

  if (deepSleepAfterStop && !streaming && !raw && !otaBusy()) {
    deepSleepAfterStop=false;
    enterDeepSleep("c3-touch-hold-after-stop");
    return;
  }
  if (standbyAfterStop && !streaming && !raw && !otaBusy()) {
    standbyAfterStop=false;
    enterRemoteStandby();
    return;
  }

  if (connected!=lastConnectedState || streaming!=lastStreamingState || standby!=lastStandbyState) {
    lastConnectedState=connected;
    lastStreamingState=streaming;
    lastStandbyState=standby;
    touchRearmAt=now+C3_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    tapCount=0;
    lastTapAt=0;
  }

  // A lone tap intentionally does nothing. Expire it after the double-tap window.
  if (tapCount==1 && lastTapAt && uint32_t(now-lastTapAt)>C3_DOUBLE_TAP_GAP_MS) {
    tapCount=0;
    lastTapAt=0;
  }

  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (otaBusy() || sleepPending || static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;
        tapCount=0;
        lastTapAt=0;
        return;
      }
      touchPressedAt=now;
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held || otaBusy()) {
      tapCount=0;lastTapAt=0;return;
    }

    // C3-only power gesture. Sleep is requested only after release, preventing
    // the level-sensitive GPIO3 wake source from immediately waking again.
    if (held>=C3_SLEEP_HOLD_MS) {
      tapCount=0;lastTapAt=0;
      touchRearmAt=now+C3_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] C3 long press -> DEEP SLEEP");
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("c3-touch-hold");
      }
      return;
    }

    if (held<C3_TAP_MIN_MS || held>C3_TAP_MAX_MS) {
      tapCount=0;lastTapAt=0;
      return;
    }

    if (!tapCount || !lastTapAt || uint32_t(now-lastTapAt)>C3_DOUBLE_TAP_GAP_MS) {
      tapCount=1;
      lastTapAt=now;
      return;
    }

    // Second valid tap acts immediately; there is no triple-tap ambiguity on C3.
    tapCount=0;lastTapAt=0;
    touchRearmAt=now+C3_STATE_LOCKOUT_MS;
    if (streamingEnabled.load()) {
      standbyAfterStop=true;
      Serial.println("[TOUCH] C3 double tap -> STOP + POWER SAVER");
      queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
    } else if (deviceConnected.load()) {
      Serial.println(remoteStandby ? "[TOUCH] C3 double tap standby -> START" : "[TOUCH] C3 double tap -> START");
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
    }
  }
}
`;
  out=replaceFunctionBlock(out,'void pollTouchControl() {','void updateStatusCharacteristic',c3Touch,'C3 awake touch gesture');

  const taskBefore=`  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {`;
  const taskAfter=`  // ESP32-C3 has one core; retain task priorities and stack sizes without pinning.
  if (xTaskCreate(controlTask, "control", 8192, nullptr, 3, nullptr) != pdPASS ||
      xTaskCreate(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle) != pdPASS ||
      xTaskCreate(transmitterTask, "transmit", 8192, nullptr, 2, nullptr) != pdPASS) {`;
  out=replaceOnce(out,taskBefore,taskAfter,'single-core task creation');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`SYNAP-FW:${target.id}:1.0.0:`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('C3 long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('C3 double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  if(out.includes('triple tap -> DEEP SLEEP'))throw Error('C3 source still contains the S3 triple-tap power gesture');
  return out;
}

if(require.main===module){
  const args=process.argv.slice(2),check=args[0]==='--check';
  if(check)args.shift();
  const [targetId,input,output]=args;
  if(!targetId||!input)throw Error('Usage: node tools/materialize-target.cjs [--check] <target> <prepared-source> [output]');
  const source=fs.readFileSync(input,'utf8'),result=materialize(source,targetId);
  if(check){console.log(`PASS: materialized ${targetId}`);process.exit(0);}
  if(!output)throw Error('Output path is required unless --check is used');
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,result);
  console.log(`Materialized ${targetId} -> ${output}`);
}

module.exports={materialize,replaceOnce,replaceFunctionBlock};
