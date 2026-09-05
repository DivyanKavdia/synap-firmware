const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  // TTP223 OUT is push-pull. Do not bias the module output with the ESP internal
  // pulldown; read the driven digital level directly.
  out=replaceOnce(out,
`  pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN);`,
`  pinMode(TOUCH_INPUT_PIN, INPUT);`,
  'TTP223 input mode');

  // A TTP223 is intentionally very sensitive, so a generic short-tap toggle is
  // too easy to trigger while the pendant is being handled or brushed by clothing.
  // Starting from idle therefore requires a deliberate press-and-release window.
  // Recording stop stays quicker, while the existing long-press Remember gesture
  // and 3 s idle sleep gesture remain distinct. State transitions add a short
  // lockout so a finger held across START/STOP cannot immediately reverse state.
  const start=out.indexOf('void pollTouchControl() {');
  const end=out.indexOf('\n}\n',start);
  if(start<0||end<0)throw new Error('Missing firmware anchor: touch control function');
  const replacement=`void pollTouchControl() {
  constexpr uint16_t TOUCH_START_HOLD_MS = 550;
  constexpr uint16_t TOUCH_START_MAX_MS = 1400;
  constexpr uint16_t TOUCH_STOP_MIN_MS = 140;
  constexpr uint16_t TOUCH_STOP_MAX_MS = 850;
  constexpr uint16_t TOUCH_REARM_MS = 650;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 900;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();

  if (connected!=lastConnectedState || streaming!=lastStreamingState) {
    lastConnectedState=connected;
    lastStreamingState=streaming;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    touchLongSent=false;
    touchLongEligible=false;
    touchIdlePress=false;
  }

  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;
        touchLongSent=false;
        touchLongEligible=false;
        touchIdlePress=false;
        Serial.println("[TOUCH] ignored during state lockout");
        return;
      }
      touchPressedAt=now;
      touchLongSent=false;
      touchIdlePress=!streaming;
      touchLongEligible=connected && streaming && !otaBusy();
      Serial.printf("[TOUCH] press gpio=%u streaming=%u connected=%u\\n",
        unsigned(TOUCH_INPUT_PIN),streaming?1u:0u,connected?1u:0u);
    } else {
      const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
      const bool wasLong=touchLongSent;
      const bool wasIdle=touchIdlePress;
      touchPressedAt=0;touchLongSent=false;touchLongEligible=false;touchIdlePress=false;

      if (held>=TOUCH_SLEEP_HOLD_MS && wasIdle && !streamingEnabled.load() && !otaBusy()) {
        touchRearmAt=now+TOUCH_REARM_MS;
        Serial.println("[TOUCH] sleep hold");
        enterDeepSleep("touch-hold");
      } else if (!wasLong && wasIdle && held>=TOUCH_START_HOLD_MS && held<=TOUCH_START_MAX_MS &&
                 deviceConnected.load() && !streamingEnabled.load() && !otaBusy()) {
        touchRearmAt=now+TOUCH_REARM_MS;
        Serial.printf("[TOUCH] deliberate start hold %ums -> START\\n",unsigned(held));
        queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
      } else if (!wasLong && !wasIdle && held>=TOUCH_STOP_MIN_MS && held<=TOUCH_STOP_MAX_MS &&
                 deviceConnected.load() && streamingEnabled.load() && !otaBusy()) {
        touchRearmAt=now+TOUCH_REARM_MS;
        Serial.printf("[TOUCH] deliberate stop tap %ums -> STOP\\n",unsigned(held));
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else if (held) {
        Serial.printf("[TOUCH] ignored gesture %ums idle=%u\\n",unsigned(held),wasIdle?1u:0u);
      }
    }
  }
  if (touchStableState && touchLongEligible && !touchLongSent && touchPressedAt &&
      uint32_t(now-touchPressedAt)>=TOUCH_LONG_PRESS_MS && deviceConnected.load() &&
      streamingEnabled.load() && !otaBusy()) {
    touchLongSent=true;
    Serial.println("[TOUCH] long press -> REMEMBER");
    publishRememberEvent();
  }
}`;
  out=out.slice(0,start)+replacement+out.slice(end+3);

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-touch-reliability.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap TTP223 reliability');
}
module.exports={patch};
