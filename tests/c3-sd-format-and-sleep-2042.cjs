'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');

const read = (name) => fs.readFileSync('firmware/shared/' + name, 'utf8');
const detect = read('odyssey-sd-1631-detect.cpp');
const recording = read('odyssey-sd-1631-recording.cpp');
const transfer = read('odyssey-sd-1631-transfer.cpp');
const caps = read('module-capabilities.cpp');
const power = read('power.cpp');
const led = read('status-led.cpp');
const assembled = fs.readFileSync('synap_esp32s3/synap_esp32s3.ino', 'utf8');

test('full-card format is an explicit guarded operation; never on failed mount', () => {
  assert.match(detect, /mount\.format_if_mount_failed=false/);
  assert.match(detect, /esp_err_t odysseyFormatSdCard\(\)/);
  const formatter = detect.slice(detect.indexOf('esp_err_t odysseyFormatSdCard()'),
    detect.indexOf('void odysseyDetectSdCard()'));
  assert.match(formatter, /OdysseySdGuard guard/);
  assert.match(formatter, /odysseySdReady\(\)/);
  assert.match(formatter, /odysseyRecording\.load\(\)/);
  assert.match(formatter, /odysseySdUnsafeToSleep\.load\(\)/);
  assert.match(formatter, /esp_vfs_fat_sdcard_format\(/);
  assert.match(formatter, /odysseySdValidateVfsLocked\("format",1\)/);
  assert.match(formatter, /errno!=ENOENT/);
  assert.match(formatter, /odysseySdProbeStage=6/);
  assert.match(transfer, /case 19:[\s\S]*?request\.offset!=0[\s\S]*?odysseySdPowerSafe\([\s\S]*?odysseyFormatSdCard\(\)/);
  assert.match(transfer, /odysseyClearRecordFailureAfterFormat\(\)/);
  assert.match(recording, /void odysseyClearRecordFailureAfterFormat\(\)[\s\S]*?odysseyPersistRecordFailure\(0,0\)/);
  assert.match(caps, /p\[16\]\|=4/);
});

test('mounted offline C3 card remains available; live capture and transfers veto sleep', () => {
  const tick = power.slice(power.indexOf('void powerTick()'), power.indexOf('void pollTouchControl()'));
  assert.match(tick, /if \(odysseyRecording\.load\(\)\) \{/);
  assert.match(tick, /if \(odysseySdReady\(\)\) return;/);
  assert.match(tick, /enterDeepSleep\("disconnected-timeout"\)/);
  assert.match(power, /if \(odysseyRecording\.load\(\) \|\| OdysseyWifi::busy\(\)\) return/);
  assert.match(power, /if \(OdysseyTransfer::busy\(\)\)/);
  assert.match(detect, /bool odysseyPrepareSdForPowerTransition\([\s\S]*?if \(odysseyRecording\.load\(\)\)/);
  assert.match(recording, /OdysseySdGuard storage/);
  assert.match(recording, /odysseyRecording=false;/);
});

test('C3 SD purple heartbeat has no minute timeout; pending STOP signals orange', () => {
  assert.match(led, /odysseyRecording\.load\(\)/);
  assert.match(led, /now-odysseyRecordingStartedAt\.load\(\)\)%1800u/);
  assert.match(led, /odysseyStopRequested\.load\(\)[\s\S]*?g=2/);
  assert.match(power, /critical battery requested offline SD STOP/);
  for (const name of ['esp_err_t odysseyFormatSdCard()', 'case 19:', 'p[16]|=4',
    'if (odysseySdReady()) return;', 'critical battery requested offline SD STOP']) {
    assert.ok(assembled.includes(name), 'generated C3 sketch missing ' + name);
  }
});
