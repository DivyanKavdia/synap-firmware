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

  // Replace the former start-on-single / stop-on-double-tap state machine with
  // one deliberate tap toggle. A minimum pulse rejects EMI/edge chatter, while
  // long press during recording remains "Remember This" and a 3 s idle hold
  // remains sleep. Long-press release never toggles the stream.
  const start=out.indexOf('void pollTouchControl() {');
  const end=out.indexOf('\n}\n',start);
  if(start<0||end<0)throw new Error('Missing firmware anchor: touch control function');
  const old=out.slice(start,end+3);
  const replacement=`void pollTouchControl() {
  constexpr uint16_t TOUCH_MIN_TAP_MS = 80;
  constexpr uint16_t TOUCH_REARM_MS = 220;
  static uint32_t touchRearmAt = 0;
  const uint32_t now=millis();
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (static_cast<int32_t>(now-touchRearmAt)<0) return;
      touchPressedAt=now;
      touchLongSent=false;
      touchIdlePress=!streamingEnabled.load();
      touchLongEligible=deviceConnected.load() && streamingEnabled.load() && !otaBusy();
      Serial.printf("[TOUCH] press gpio=%u streaming=%u connected=%u\\n",
        unsigned(TOUCH_INPUT_PIN),streamingEnabled.load()?1u:0u,deviceConnected.load()?1u:0u);
    } else {
      const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
      const bool wasLong=touchLongSent;
      const bool wasIdle=touchIdlePress;
      touchPressedAt=0;touchLongSent=false;touchLongEligible=false;touchIdlePress=false;
      if (held>=TOUCH_SLEEP_HOLD_MS && wasIdle && !streamingEnabled.load() && !otaBusy()) {
        touchRearmAt=now+TOUCH_REARM_MS;
        Serial.println("[TOUCH] sleep hold");
        enterDeepSleep("touch-hold");
      } else if (!wasLong && held>=TOUCH_MIN_TAP_MS && held<TOUCH_LONG_PRESS_MS &&
                 deviceConnected.load() && !otaBusy()) {
        touchRearmAt=now+TOUCH_REARM_MS;
        const uint8_t command=streamingEnabled.load()?CMD_STOP:CMD_START;
        Serial.printf("[TOUCH] tap %ums -> %s\\n",unsigned(held),command==CMD_START?"START":"STOP");
        queueEvent(EventType::COMMAND,command,PROTOCOL_VERSION,streamGeneration.load());
      } else if (held && held<TOUCH_MIN_TAP_MS) {
        Serial.printf("[TOUCH] ignored short pulse %ums\\n",unsigned(held));
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
