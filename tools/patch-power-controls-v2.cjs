'use strict';
const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing power-controls anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous power-controls anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}
function replaceFunction(source,signature,replacement,label){
  const start=source.indexOf(signature);
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
constexpr uint8_t POWER_STATE_DEEP_SLEEP = 3;`,
  'power command constants');

  out=replaceOnce(out,
`enum class DeviceState : uint8_t { DISCONNECTED=0, CONNECTED_IDLE=1, STREAMING=2, ERROR=3 };`,
`enum class DeviceState : uint8_t { DISCONNECTED=0, CONNECTED_IDLE=1, STREAMING=2, ERROR=3, STANDBY=4 };`,
  'standby device state');

  out=replaceOnce(out,
`bool restartAdvertising = false;`,
`bool restartAdvertising = false;
bool remoteStandby = false;`,
  'remote standby state');

  // Keep the status LED completely off in BLE standby. Without this explicit arm,
  // the generic "other state" error pattern would flash purple and waste power.
  out=replaceOnce(out,
`  } else if (deviceState == DeviceState::STREAMING) {`,
`  } else if (deviceState == DeviceState::STANDBY) {
    // BLE stays alive for remote wake; the visible LED intentionally stays off.
  } else if (deviceState == DeviceState::STREAMING) {`,
  'standby LED off');

  // Replace the old 5-second wake confirmation with a true single-touch wake.
  // Consume the physical wake touch so it cannot become the first tap of a
  // double-tap recording gesture after boot.
  out=replaceFunction(out,'bool confirmTouchWakeHold()',`bool confirmTouchWakeHold() {
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bool touchWake=(cause==ESP_SLEEP_WAKEUP_EXT1);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  touchWake=touchWake || (cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
  if (!touchWake) return true;
  Serial.println("[TOUCH] single touch wake");
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) delay(8);
  touchRawState=false;touchStableState=false;touchPressedAt=0;touchFirstTapAt=0;
  touchChangedAt=millis();
  return true;
}`,'single-touch deep-sleep wake');

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
#if USE_REAL_I2S_MIC
  if (!startMicrophone()) {
    remoteStandby=false;
    setDeviceState(DeviceState::ERROR, ErrorCode::AUDIO_SOURCE_FAILED);
    updateStatusCharacteristic(true);
    return false;
  }
#endif
  remoteStandby=false;
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  configureTransportFromPeerMtu();
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_AWAKE);
  Serial.println("[POWER] remote standby -> awake");
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
  setDeviceState(DeviceState::STANDBY, ErrorCode::NONE);
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_STANDBY);
  statusLed.clear();statusLed.show();
  Serial.println("[POWER] remote standby; BLE remains available");
}

`;
  out=out.slice(0,sleepAt)+helpers+out.slice(sleepAt);

  // Publish the final true-deep-sleep state before BLE is deinitialized. The PWA
  // can persist it and distinguish an intentional sleep disconnect from link loss.
  const sleepBodyAnchor=`void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load()) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;`;
  out=replaceOnce(out,sleepBodyAnchor,
`void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load()) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;
  remoteStandby=false;
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
      break;
    case CMD_STOP:
      if (remoteStandby) updateStatusCharacteristic(true);
      else stopStreaming();
      break;
    case CMD_GET_STATUS:
      if (remoteStandby) {
        setDeviceState(DeviceState::STANDBY, ErrorCode::NONE);
      } else if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      sampleBattery(true);
      break;
    case CMD_STANDBY:
      enterRemoteStandby();
      break;
    case CMD_WAKE:
      exitRemoteStandby();
      break;
    default:
      stopStreaming(ErrorCode::BAD_COMMAND);
      break;
  }
}`,'power-aware command handling');

  // Preserve any existing CONNECTED-case preparation (battery and transport
  // bookkeeping) and replace only its normal stop action with standby-aware logic.
  {
    const caseStart=out.indexOf('case EventType::CONNECTED:');
    const caseEnd=out.indexOf('case EventType::DISCONNECTED:',caseStart);
    if(caseStart<0||caseEnd<0)throw new Error('Missing power-controls anchor: CONNECTED event arm');
    let arm=out.slice(caseStart,caseEnd);
    const stopAt=arm.indexOf('stopStreaming();');
    if(stopAt<0)throw new Error('Missing power-controls anchor: CONNECTED stop action');
    const replacement=`if (remoteStandby) {
            setDeviceState(DeviceState::STANDBY, ErrorCode::NONE);
            updateStatusCharacteristic(true);
            publishPowerEvent(POWER_STATE_STANDBY);
          } else {
            stopStreaming();
            publishPowerEvent(POWER_STATE_AWAKE);
          }`;
    arm=arm.slice(0,stopAt)+replacement+arm.slice(stopAt+'stopStreaming();'.length);
    out=out.slice(0,caseStart)+arm+out.slice(caseEnd);
  }

  // Unified TTP223 model:
  // * deep sleep: any single touch wakes (handled at boot above)
  // * awake: double tap toggles START/STOP
  // * remote standby: one physical tap wakes
  // * any awake state: >=5 s hold -> true deep sleep
  out=replaceFunction(out,'void pollTouchControl()',`void pollTouchControl() {
  constexpr uint16_t TOUCH_TAP_MIN_MS = 80;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 450;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 650;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool sleepAfterStop = false;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  if (sleepAfterStop && !streaming && !raw && !otaBusy()) {
    sleepAfterStop=false;
    enterDeepSleep("touch-hold-after-stop");
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
        sleepAfterStop=true;
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

    if (remoteStandby) {
      touchFirstTapAt=0;
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] single tap -> wake from remote standby");
      exitRemoteStandby();
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
        Serial.println("[TOUCH] double tap -> STOP");
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else if (connected) {
        Serial.println("[TOUCH] double tap -> START");
        queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
      }
    } else {
      touchFirstTapAt=now;
    }
  }
}`,'unified double-tap touch controls');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-power-controls-v2.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap power controls v2: double-tap record, single-touch wake, BLE standby');
}
module.exports={patch,replaceOnce,replaceFunction};
