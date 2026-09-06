'use strict';
const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing power-controls anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous power-controls anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}
function replaceFunction(source,signature,replacement,label){
  const definition=signature.endsWith('{')?signature:signature+' {';
  const start=source.indexOf(definition);
  if(start<0)throw new Error(`Missing power-controls function: ${label}`);
  const brace=source.indexOf('{',start);
  if(brace<0)throw new Error(`Missing opening brace: ${label}`);
  let depth=0,end=-1;
  for(let i=brace;i<source.length;i++){
    if(source[i]==='{')depth++;
    else if(source[i]==='}'&&--depth===0){end=i+1;break;}
  }
  if(end<0)throw new Error(`Missing closing brace: ${label}`);
  return source.slice(0,start)+replacement+source.slice(end);
}

function patch(source){
  let out=source;

  out=replaceOnce(out,
`constexpr uint8_t CMD_GET_STATUS = 0x02;`,
`constexpr uint8_t CMD_GET_STATUS = 0x02;
constexpr uint8_t CMD_STANDBY = 0x03;
constexpr uint8_t CMD_WAKE = 0x04;
constexpr uint8_t POWER_EVENT_MAGIC = 0xE2;
constexpr uint8_t POWER_EVENT_VERSION = 1;
constexpr uint8_t POWER_STATE_AWAKE = 1;
constexpr uint8_t POWER_STATE_STANDBY = 2;
constexpr uint8_t POWER_STATE_DEEP_SLEEP = 3;
constexpr uint8_t POWER_STATE_WAKE_RECORD = 4;`,
  'power command constants');

  out=replaceOnce(out,
`bool restartAdvertising = false;`,
`bool restartAdvertising = false;
bool remoteStandby = false;
bool wakeRecordIntent = false;`,
  'power state');

  out=replaceOnce(out,
`  } else if (deviceState == DeviceState::CONNECTED_IDLE) {`,
`  } else if (remoteStandby) {
    // BLE stays connected while mic and LED are off.
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {`,
  'standby LED off without new status state');

  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC
  if (reason==ErrorCode::AUDIO_SOURCE_FAILED && microphoneReady) stopMicrophone();
#endif
  applyCpuPowerProfile(false);`,
`#if USE_REAL_I2S_MIC
  if (microphoneReady) { vTaskDelay(pdMS_TO_TICKS(90)); stopMicrophone(); }
#endif
  applyCpuPowerProfile(false);`,
  'normal stop shuts microphone');

  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC
  microphoneValidated=startMicrophone();
#endif
  applyCpuPowerProfile(false);`,
`#if USE_REAL_I2S_MIC
  microphoneValidated=startMicrophone();
  if (microphoneValidated) stopMicrophone();
#endif
  applyCpuPowerProfile(false);`,
  'boot validation shuts microphone');

  out=replaceFunction(out,'bool confirmTouchWakeHold()',`bool confirmTouchWakeHold() {
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bool touchWake=(cause==ESP_SLEEP_WAKEUP_EXT1);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  touchWake=touchWake || (cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
  if (!touchWake) return true;

  Serial.println("[TOUCH] wake detected; hold for 5 seconds to stay awake");
  const uint32_t started=millis();
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
    if (uint32_t(millis()-started)>=TOUCH_SLEEP_HOLD_MS) {
      Serial.println("[TOUCH] 5 second wake hold confirmed");
      while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) delay(10);
      touchRawState=false;touchStableState=false;touchPressedAt=0;touchFirstTapAt=0;
      touchChangedAt=millis();
      wakeRecordIntent=false;
      return true;
    }
    delay(10);
  }

  Serial.println("[TOUCH] wake hold too short; returning to deep sleep");
  delay(40);
  armTouchWakeAndSleep();
  return false;
}`,'five-second deep-sleep wake hold');

  const sleepSignature='void enterDeepSleep(const char* reason) {';
  const sleepAt=out.indexOf(sleepSignature);
  if(sleepAt<0)throw new Error('Missing power-controls anchor: deep sleep function');
  const helpers=`void publishPowerEvent(uint8_t powerState) {
  if (!eventCharacteristic || !deviceConnected.load()) return;
  uint8_t value[6] = {POWER_EVENT_MAGIC, POWER_EVENT_VERSION, powerState,
    static_cast<uint8_t>(deviceState),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD & 255),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD >> 8)};
  eventCharacteristic->setValue(value,sizeof(value));
  eventCharacteristic->notify();
}

bool exitRemoteStandby() {
  if (!remoteStandby) return true;
  if (otaBusy()) return false;
  remoteStandby=false;
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  configureTransportFromPeerMtu();
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_AWAKE);
  Serial.println("[POWER] remote standby -> awake; microphone remains off until START");
  return true;
}

void enterRemoteStandby() {
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (streamingEnabled.load()) stopStreaming();
  remoteStandby=true;
#if USE_REAL_I2S_MIC
  if (microphoneReady) stopMicrophone();
#endif
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_STANDBY);
  statusLed.clear();statusLed.show();
  Serial.println("[POWER] remote standby; BLE available, mic/I2S off");
}

`;
  out=out.slice(0,sleepAt)+helpers+out.slice(sleepAt);

  const sleepBodyAnchor=`void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load()) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;`;
  out=replaceOnce(out,sleepBodyAnchor,
`void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load()) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;
  remoteStandby=false;
  wakeRecordIntent=false;
  publishPowerEvent(POWER_STATE_DEEP_SLEEP);
  if (deviceConnected.load()) delay(140);`,
  'deep sleep power event');

  out=replaceFunction(out,'void processCommand(uint8_t command, uint8_t version)',`void processCommand(uint8_t command, uint8_t version) {
  if (!deviceConnected.load()) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  switch (command) {
    case CMD_START:
      if (remoteStandby && !exitRemoteStandby()) break;
      startStreaming(version);
      if (streamingEnabled.load()) {
        wakeRecordIntent=false;
        publishPowerEvent(POWER_STATE_AWAKE);
      }
      break;
    case CMD_STOP:
      if (remoteStandby) updateStatusCharacteristic(true);
      else stopStreaming();
      break;
    case CMD_GET_STATUS:
      if (remoteStandby) {
        // Standby remains CONNECTED_IDLE on protocol v2.
        setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
      } else if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      sampleBattery(true);
      break;
    case CMD_STANDBY:
      if (!streamingEnabled.load()) enterRemoteStandby();
      else updateStatusCharacteristic(true);
      break;
    case CMD_WAKE:
      exitRemoteStandby();
      break;
    default:
      stopStreaming(ErrorCode::BAD_COMMAND);
      break;
  }
}`,'power-aware command handling');

  {
    const caseStart=out.indexOf('case EventType::CONNECTED:');
    const caseEnd=out.indexOf('case EventType::DISCONNECTED:',caseStart);
    if(caseStart<0||caseEnd<0)throw new Error('Missing power-controls anchor: CONNECTED event arm');
    let arm=out.slice(caseStart,caseEnd);
    const stopAt=arm.indexOf('stopStreaming();');
    if(stopAt<0)throw new Error('Missing power-controls anchor: CONNECTED stop action');
    const replacement=`if (remoteStandby) {
            setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
            updateStatusCharacteristic(true);
            publishPowerEvent(POWER_STATE_STANDBY);
          } else {
            stopStreaming();
            publishPowerEvent(wakeRecordIntent ? POWER_STATE_WAKE_RECORD : POWER_STATE_AWAKE);
          }`;
    arm=arm.slice(0,stopAt)+replacement+arm.slice(stopAt+'stopStreaming();'.length);
    out=out.slice(0,caseStart)+arm+out.slice(caseEnd);
  }

  out=replaceFunction(out,'void pollTouchControl()',`void pollTouchControl() {
  constexpr uint16_t TOUCH_TAP_MIN_MS = 80;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 450;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 650;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool standbyAfterStop = false;
  static bool deepSleepAfterStop = false;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  if (deepSleepAfterStop && !streaming && !raw && !otaBusy()) {
    deepSleepAfterStop=false;
    enterDeepSleep("touch-hold-after-stop");
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
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    touchFirstTapAt=0;
  }

  if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)>TOUCH_DOUBLE_TAP_MS) touchFirstTapAt=0;
  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;touchFirstTapAt=0;
        return;
      }
      touchPressedAt=now;
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held) return;

    if (held>=TOUCH_SLEEP_HOLD_MS && !otaBusy()) {
      touchFirstTapAt=0;
      Serial.printf("[TOUCH] %ums hold -> DEEP SLEEP\\n",unsigned(held));
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("touch-hold");
      }
      return;
    }

    if (held<TOUCH_TAP_MIN_MS || held>TOUCH_TAP_MAX_MS || otaBusy()) {
      touchFirstTapAt=0;
      return;
    }

    if (!touchFirstTapAt) {
      touchFirstTapAt=now;
      return;
    }

    if (uint32_t(now-touchFirstTapAt)<=TOUCH_DOUBLE_TAP_MS) {
      touchFirstTapAt=0;
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      if (streamingEnabled.load()) {
        standbyAfterStop=true;
        Serial.println("[TOUCH] double tap -> STOP + POWER SAVER");
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else if (connected) {
        Serial.println(remoteStandby ? "[TOUCH] double tap standby -> START" : "[TOUCH] double tap -> START");
        queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
      }
    } else {
      touchFirstTapAt=now;
    }
  }
}`,'unified touch controls');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-power-controls-v2.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap power controls v2: 5s sleep/wake, double-tap start/stop, standby');
}
module.exports={patch,replaceOnce,replaceFunction};
