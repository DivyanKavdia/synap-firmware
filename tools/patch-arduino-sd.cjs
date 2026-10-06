'use strict';
// C3-only reliability patch for Arduino-ESP32 3.3.5 SD/SPI.
//
// The stock single-sector CMD24 path reads the data-response token, raises CS,
// then depends on a later command selection to observe the card's programming
// busy period. ESP-IDF's SDSPI host instead keeps CS asserted and waits for
// programming to complete for every block, with a 5 second write timeout.
//
// Synap's offline PCM path uses multi-block CMD25 successfully. FAT metadata
// written by fclose() is single-sector, so this patch aligns CMD24 completion
// with the native ESP-IDF behavior without changing the recorder or S3 builds.
const fs = require('node:fs');

const before = `bool sdWriteSector(uint8_t pdrv, const char *buffer, unsigned long long sector) {
  for (int f = 0; f < 3; f++) {
    if (!sdSelectCard(pdrv)) {
      return false;
    }
    if (!sdCommand(pdrv, WRITE_BLOCK_SINGLE, (s_cards[pdrv]->type == CARD_SDHC) ? sector : sector << 9, NULL)) {
      char token = sdWriteBytes(pdrv, buffer, 0xFE);
      sdDeselectCard(pdrv);

      if (token == 0x0A) {
        continue;
      } else if (token == 0x0C) {
        return false;
      }

      unsigned int resp;
      if (sdTransaction(pdrv, SEND_STATUS, 0, &resp) || resp) {
        return false;
      }
      return true;
    } else {
      break;
    }
  }
  sdDeselectCard(pdrv);
  return false;
}`;

const after = `bool sdWriteSector(uint8_t pdrv, const char *buffer, unsigned long long sector) {
  for (int f = 0; f < 3; f++) {
    if (!sdSelectCard(pdrv)) {
      return false;
    }
    if (!sdCommand(pdrv, WRITE_BLOCK_SINGLE, (s_cards[pdrv]->type == CARD_SDHC) ? sector : sector << 9, NULL)) {
      char token = sdWriteBytes(pdrv, buffer, 0xFE);

      // SYNAP_SD_CMD24_BUSY_FIX:
      // 0x05 is the only accepted SD data-response token. Keep CS asserted
      // while the card programs the block, matching ESP-IDF SDSPI semantics.
      if (token != 0x05) {
        sdDeselectCard(pdrv);
        // CRC error is retryable; write/unknown errors are not.
        if (token == 0x0B) {
          continue;
        }
        return false;
      }
      if (!sdWait(pdrv, 5000)) {
        sdDeselectCard(pdrv);
        return false;
      }
      sdDeselectCard(pdrv);

      unsigned int resp;
      if (sdTransaction(pdrv, SEND_STATUS, 0, &resp) || resp) {
        return false;
      }
      return true;
    } else {
      break;
    }
  }
  sdDeselectCard(pdrv);
  return false;
}`;

function patch(source) {
  if (source.includes('SYNAP_SD_CMD24_BUSY_FIX')) return source;
  if (source.split(before).length !== 2) {
    throw new Error('Pinned Arduino SD patch no longer matches ESP32 core 3.3.5');
  }
  return source.replace(before, after);
}

function install(file) {
  const source = fs.readFileSync(file, 'utf8');
  const output = patch(source);
  fs.writeFileSync(file, output);
}

if (require.main === module) {
  if (!process.argv[2]) {
    throw new Error('Usage: node tools/patch-arduino-sd.cjs <esp32-3.3.5/libraries/SD/src/sd_diskio.cpp>');
  }
  install(process.argv[2]);
  console.log('Applied pinned C3 CMD24 busy-completion fix');
}

module.exports = { patch, install, before, after };
