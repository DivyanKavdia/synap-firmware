'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const source=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const assembled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
for (const [name,body] of [['shared',source],['assembled',assembled]]) {
  test(name+': native read timeout during FAT mount quarantines before card pointer is returned',()=>{
    const tx=body.split('host.do_transaction=[]')[1].split('sdspi_device_config_t device')[0];
    assert.match(tx,/error==ESP_ERR_TIMEOUT\s*&&\s*\(cmd->opcode==17u \|\| cmd->opcode==18u\)/);
    assert.doesNotMatch(tx,/odysseySdBootState\.load\(\)==1 \|\| odysseySdNativeCard!=nullptr/);
  });
  test(name+': mounted timeout refuses bus free and FatFs unmount',()=>{
    const release=body.split('static bool odysseySdReleaseLocked() {')[1].split('static uint8_t odysseySdBitBangTransfer')[0];
    assert.ok(release.indexOf('SPI2 release refused: native timeout quarantine')>=0);
    assert.ok(release.indexOf('SPI2 release refused: native timeout quarantine')<
      release.indexOf('esp_vfs_fat_sdcard_unmount('));
    assert.ok(release.indexOf('SPI2 release refused: native timeout quarantine')<
      release.indexOf('spi_bus_free('));
  });
  test(name+': idle failure never unmounts/clears mount state on sleep or OTA path',()=>{
    const deep=body.split('bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {')[1]
      .split('// A committed OTA has already selected')[0];
    const ota=body.split('bool odysseyPrepareSdForCommittedOtaRestart(uint32_t timeoutMs) {')[1]
      .split('#else\n// Odyssey S3 remains detection-only')[0];
    for (const path of [deep,ota]) {
      const check=path.split('const uint8_t idle=odysseySdQuiesceLocked(')[1];
      assert.ok(check, 'quiesce is required for a mounted card');
      const fail=check.split('if (idle!=1) {')[1].split('return false;')[0];
      assert.doesNotMatch(fail,/odysseySdReleaseLocked\(/);
      assert.match(fail,/odysseySdRememberRailUnsafe\(true\)/);
      assert.ok(check.indexOf('if (idle!=1) {')<check.indexOf('odysseySdReleaseLocked()'));
    }
  });
  test(name+': persistent same-rail warm boot veto is cleared only on successful cold boot',()=>{
    assert.match(body,/prefs\.getUChar\("railBad",0\)/);
    assert.match(body,/prefs\.putUChar\("railBad",unsafe\?1u:0u\)/);
    assert.match(body,/odysseySdCleanupUnsafePersisted\(\) \|\| odysseySdRailUnsafePersisted\(\)/);
    const boot=body.split('bool odysseyInitializeSdCardBeforeBle() {')[1]
      .split('bool odysseyRecoverSdCard(')[0];
    assert.match(boot,/if \(ready && bootResetReason==ESP_RST_POWERON\)/);
    assert.match(boot,/odysseySdRememberRailUnsafe\(false\)/);
  });
}

for (const [name,body] of [['shared',transfer],['assembled',assembled]]) {
  test(name+': BLE read timeout strands descriptor instead of calling fclose',()=>{
    const read=body.split('static uint8_t readWavChunk(')[1]
      .split('static uint8_t readSelected(')[0];
    assert.match(read,/const bool nativeReadTimeout=odysseySdReadTimeoutQuarantined\(\)/);
    assert.match(read,/nativeReadTimeout\?-1:fclose\(file\)/);
    assert.match(read,/if \(!seekOk \|\| got!=want \|\| closeResult!=0 \|\| nativeReadTimeout\)/);
  });
}
