'use strict';
// Backport Espressif arduino-esp32 PR #12766 to the pinned 3.3.5 SD library.
// Keep the core pinned; fail closed if the expected 3.3.5 source shape changes.
const fs=require('node:fs'),path=require('node:path');

function replaceOnce(source,before,after,label){
  if(source.split(before).length!==2)throw Error('Pinned SD patch no longer matches: '+label);
  return source.replace(before,after);
}
function patchInitialization(source){
  if(source.includes('static constexpr uint32_t sd_go_idle_delay_ms = 20;') &&
     source.includes('APP_OP_COND, 0x40000000') &&
     source.includes('sd_op_cond_timeout_ms')) return source;

  source=replaceOnce(source,
`  CRC_ON_OFF = 59
} ardu_sdcard_command_t;

typedef struct {`,
`  CRC_ON_OFF = 59
} ardu_sdcard_command_t;

// Backport of Espressif arduino-esp32 PR #12766.
static constexpr uint32_t sd_go_idle_delay_ms = 20;
static constexpr uint32_t sd_op_cond_timeout_ms = 3000;

typedef struct {`,'constants');

  source=replaceOnce(source,
`  // Step 2: Select the card and send GO_IDLE_STATE command
  // This command resets the card to idle state and enables SPI mode
  // Fix mount issue - sdWait fail ignored before command GO_IDLE_STATE
  digitalWrite(card->ssPin, LOW);
  if (!sdWait(pdrv, 500)) {
    log_w("sdWait fail ignored, card initialize continues");
  }
  if (sdCommand(pdrv, GO_IDLE_STATE, 0, NULL) != 1) {
    sdDeselectCard(pdrv);
    log_w("GO_IDLE_STATE failed");
    goto unknown_card;
  }
  sdDeselectCard(pdrv);

  // Step 3: Configure CRC checking
  // Enable CRC for data transfers in SPI mode (required for reliable communication)
  token = sdTransaction(pdrv, CRC_ON_OFF, 1, NULL);
  if (token == 0x5) {`,
`  // Step 2: perform two CMD0 attempts with a delay. Some cards only
  // enter SPI mode after the first CMD0 and answer the second one.
  digitalWrite(card->ssPin, LOW);
  if (!sdWait(pdrv, 500)) {
    log_w("sdWait fail ignored, card initialize continues");
  }
  (void)sdCommand(pdrv, GO_IDLE_STATE, 0, NULL);
  sdDeselectCard(pdrv);
  delay(sd_go_idle_delay_ms);

  digitalWrite(card->ssPin, LOW);
  if (!sdWait(pdrv, 500)) {
    log_w("sdWait fail ignored, card initialize continues");
  }
  if (sdCommand(pdrv, GO_IDLE_STATE, 0, NULL) != 1) {
    sdDeselectCard(pdrv);
    log_w("GO_IDLE_STATE failed");
    goto unknown_card;
  }
  sdDeselectCard(pdrv);
  delay(sd_go_idle_delay_ms);

  // Step 3: enable CRC, retrying once as ESP-IDF does.
  token = sdTransaction(pdrv, CRC_ON_OFF, 1, NULL);
  if (token != 1 && token != 0x5) {
    delay(10);
    token = sdTransaction(pdrv, CRC_ON_OFF, 1, NULL);
  }
  if (token == 0x5) {`,'CMD0/CRC');

  source=replaceOnce(source,
`    // Send APP_OP_COND to set operating conditions for SDHC/SDXC
    // Wait up to 1 second for the card to become ready
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0x40100000, NULL);
    } while (token == 1 && (millis() - start) < 1000);`,
`    // SPI mode ACMD41: only HCS (bit 30) is valid; allow >1 s.
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0x40000000, NULL);
    } while (token == 1 && (millis() - start) < sd_op_cond_timeout_ms);`,'SDHC ACMD41');

  source=replaceOnce(source,
`    // Try SD card initialization first
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0x100000, NULL);
    } while (token == 0x01 && (millis() - start) < 1000);`,
`    // Standard-capacity SD in SPI mode: ACMD41 argument must be zero.
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0, NULL);
    } while (token == 0x01 && (millis() - start) < sd_op_cond_timeout_ms);`,'SDSC ACMD41');

  source=replaceOnce(source,
`      // Try MMC card initialization
      start = millis();
      do {
        token = sdTransaction(pdrv, SEND_OP_COND, 0x100000, NULL);
      } while (token != 0x00 && (millis() - start) < 1000);`,
`      // MMC SPI mode CMD1 argument must be zero.
      start = millis();
      do {
        token = sdTransaction(pdrv, SEND_OP_COND, 0, NULL);
      } while (token != 0x00 && (millis() - start) < sd_op_cond_timeout_ms);`,'MMC CMD1');

  return source;
}
// Apply the compatibility fix only to C3; preserve the other targets' driver.
function patch(source){
  if(source.includes('// SYNAP_C3_SD_INIT_V2')) return source;
  const start=source.indexOf('DSTATUS ff_sd_initialize(uint8_t pdrv) {');
  const end=source.indexOf('DSTATUS ff_sd_status(',start);
  if(start<0 || end<0) throw Error('Pinned SD initialization boundary missing');
  const original=source.slice(start,end);
  let fixed=patchInitialization(source);
  const fixedStart=fixed.indexOf('DSTATUS ff_sd_initialize(uint8_t pdrv) {');
  const fixedEnd=fixed.indexOf('DSTATUS ff_sd_status(',fixedStart);
  let init=fixed.slice(fixedStart,fixedEnd);
  init=replaceOnce(init,'  char token;',
    '  synap_sd_tracking = true;\n  char token;','diagnostic start');
  for(let step=1;step<=7;step++) {
    const marker='  // Step '+step+':';
    init=replaceOnce(init,marker,'  synap_sd_stage = '+step+';\n'+marker,'stage '+step);
  }
  init=init.replaceAll('  return card->status;',
    '  synap_sd_tracking = false;\n  return card->status;');
  fixed=fixed.slice(0,fixedStart)+'#if CONFIG_IDF_TARGET_ESP32C3\n'+init+
    '#else\n'+original+'#endif\n\n'+fixed.slice(fixedEnd);
  const diagnostics=`// SYNAP_C3_SD_INIT_V2
// Read only after SD.begin returns, under the application's storage mutex.
// Stop tracking before SD.begin failure cleanup sends a fresh CMD0.
#if CONFIG_IDF_TARGET_ESP32C3
static int synap_sd_stage=0, synap_sd_cmd=-1, synap_sd_r1=-1;
static int synap_sd_fat=-1, synap_sd_vfs=0;
static bool synap_sd_tracking=false;
extern "C" void synap_sd_reset_diagnostics() {
  synap_sd_stage=0; synap_sd_cmd=-1; synap_sd_r1=-1;
  synap_sd_fat=-1; synap_sd_vfs=0; synap_sd_tracking=false;
}
extern "C" int synap_sd_diagnostic(unsigned field) {
  switch(field) {
    case 0: return synap_sd_stage;
    case 1: return synap_sd_cmd;
    case 2: return synap_sd_r1;
    case 3: return synap_sd_fat;
    case 4: return synap_sd_vfs;
    default: return -1;
  }
}
#endif

`;
  fixed=replaceOnce(fixed,'typedef struct {',diagnostics+'typedef struct {','diagnostic storage');
  fixed=replaceOnce(fixed,'  return token;\n}\n\nbool sdReadBytes',
`#if CONFIG_IDF_TARGET_ESP32C3
  if (synap_sd_tracking) { synap_sd_cmd=uint8_t(cmd); synap_sd_r1=uint8_t(token); }
#endif
  return token;
}

bool sdReadBytes`,'command result');
  fixed=replaceOnce(fixed,'  esp_err_t err = esp_vfs_fat_register(path, drv, max_files, &fs);',
`  esp_err_t err = esp_vfs_fat_register(path, drv, max_files, &fs);
#if CONFIG_IDF_TARGET_ESP32C3
  synap_sd_vfs=err;
#endif`,'VFS result');
  fixed=replaceOnce(fixed,'  FRESULT res = f_mount(fs, drv, 1);',
`  FRESULT res = f_mount(fs, drv, 1);
#if CONFIG_IDF_TARGET_ESP32C3
  synap_sd_fat=res;
#endif`,'FAT result');
  // A write is accepted only on SD data-response token 0x05. Stock 3.3.5
  // rejects explicit CRC/write errors but can accept timeout/unknown tokens
  // if the subsequent status command succeeds, silently losing a sector.
  fixed=replaceOnce(fixed,
`      } else if (token == 0x0C) {
        return false;
      }

      unsigned int resp;`,
`      } else if (token == 0x0C) {
        return false;
      }
#if CONFIG_IDF_TARGET_ESP32C3
      if (token != 0x05) return false;
#endif

      unsigned int resp;`,'single-sector write acceptance');
  return fixed;
}
function install(directory){
  const file=path.join(directory,'sd_diskio.cpp');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
}
if(require.main===module){
  if(!process.argv[2])throw Error('Usage: node tools/patch-arduino-sd.cjs <esp32-3.3.5/libraries/SD/src>');
  install(process.argv[2]);
  console.log('Applied pinned Arduino ESP32 3.3.5 SD SPI initialization backport');
}
module.exports={patch,install};
