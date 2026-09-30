'use strict';
// Backport Espressif arduino-esp32 PR #12766 to the pinned 3.3.5 SD library.
// Keep the core pinned; fail closed if the expected 3.3.5 source shape changes.
const fs=require('node:fs'),path=require('node:path');

function replaceOnce(source,before,after,label){
  if(source.split(before).length!==2)throw Error('Pinned SD patch no longer matches: '+label);
  return source.replace(before,after);
}
function patch(source){
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
