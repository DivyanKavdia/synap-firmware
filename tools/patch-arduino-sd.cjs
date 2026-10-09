'use strict';
// C3-only reliability patch for Arduino-ESP32 3.3.5 SD/SPI.
//
// The stock single-sector CMD24 path reads the data-response token, raises CS,
// then depends on a later command selection to observe the card's programming
// busy period. ESP-IDF's SDSPI host instead keeps CS asserted and waits for
// programming to complete for every block, with a 5 second write timeout.
//
// Synap's 4 KiB recording uses CMD25 multi-block writes. In core 3.3.5,
// CMD25 sends a stop token then immediately raises CS (before the card is
// ready) and its error path incorrectly issues CMD12 (read-stop). This C3-only
// patch bounds all CMD25 busy waits to 5 s, sends 0xFD write-stop, and
// checks the card is ready before deselect and CMD13 status.
// S3 and Chakshu builds compile BEFORE applying this patch.
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

// Exact pinned Arduino-ESP32 3.3.5 CMD25 source. Fail closed on core drift.
const multiBefore = "bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count) {\n  char token;\n  const char *currentBuffer = buffer;\n  unsigned long long currentSector = sector;\n  int currentCount = count;\n  ardu_sdcard_t *card = s_cards[pdrv];\n\n  for (int f = 0; f < 3;) {\n    if (card->type != CARD_MMC) {\n      if (sdTransaction(pdrv, SET_WR_BLK_ERASE_COUNT, currentCount, NULL)) {\n        return false;\n      }\n    }\n\n    if (!sdSelectCard(pdrv)) {\n      return false;\n    }\n\n    if (!sdCommand(pdrv, WRITE_BLOCK_MULTIPLE, (card->type == CARD_SDHC) ? currentSector : currentSector << 9, NULL)) {\n      do {\n        token = sdWriteBytes(pdrv, currentBuffer, 0xFC);\n        if (token != 0x05) {\n          f++;\n          break;\n        }\n        currentBuffer += 512;\n        f = 0;\n      } while (--currentCount);\n\n      if (!sdWait(pdrv, 500)) {\n        break;\n      }\n\n      if (currentCount == 0) {\n        sdStop(pdrv);\n        sdDeselectCard(pdrv);\n\n        unsigned int resp;\n        if (sdTransaction(pdrv, SEND_STATUS, 0, &resp) || resp) {\n          return false;\n        }\n        return true;\n      } else {\n        if (sdCommand(pdrv, STOP_TRANSMISSION, 0, NULL)) {\n          break;\n        }\n\n        if (token == 0x0A) {\n          sdDeselectCard(pdrv);\n          unsigned int writtenBlocks = 0;\n          if (card->type != CARD_MMC && sdSelectCard(pdrv)) {\n            if (!sdCommand(pdrv, SEND_NUM_WR_BLOCKS, 0, NULL)) {\n              char acmdData[4];\n              if (sdReadBytes(pdrv, acmdData, 4)) {\n                writtenBlocks = acmdData[0] << 24;\n                writtenBlocks |= acmdData[1] << 16;\n                writtenBlocks |= acmdData[2] << 8;\n                writtenBlocks |= acmdData[3];\n              }\n            }\n            sdDeselectCard(pdrv);\n          }\n          currentBuffer = buffer + (writtenBlocks << 9);\n          currentSector = sector + writtenBlocks;\n          currentCount = count - writtenBlocks;\n          continue;\n        } else {\n          break;\n        }\n      }\n    } else {\n      break;\n    }\n  }\n  sdDeselectCard(pdrv);\n  return false;\n}";
const multiAfter = "bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count) {\n  const char *currentBuffer = buffer;\n  unsigned long long currentSector = sector;\n  int currentCount = count;\n  ardu_sdcard_t *card = s_cards[pdrv];\n  if (count <= 1) {\n    return false;\n  }\n\n  if (card->type != CARD_MMC) {\n    if (sdTransaction(pdrv, SET_WR_BLK_ERASE_COUNT, currentCount, NULL)) {\n      return false;\n    }\n  }\n\n  if (!sdSelectCard(pdrv)) {\n    return false;\n  }\n  if (sdCommand(pdrv, WRITE_BLOCK_MULTIPLE, (card->type == CARD_SDHC) ? currentSector : currentSector << 9, NULL)) {\n    sdDeselectCard(pdrv);\n    return false;\n  }\n\n  // SYNAP_SD_CMD25_BUSY_FIX: CMD25 accepts 0xFC data blocks and terminates\n  // with 0xFD, never a CMD12 read-stop command. Keep CS LOW until the stop\n  // token has finished its internal flash programming (bounded to 5 seconds).\n  // Do not blindly retry a partially written FAT sector range.\n  bool accepted = true;\n  char rejectedToken = 0x05;\n  do {\n    // Stock Arduino only waits 500 ms before a block; a card busy with flash\n    // erase/program is permitted more time, without changing SPI frequency.\n    if (!sdWait(pdrv, 5000)) {\n      log_e(\"CMD25 block-ready timeout\");\n      sdDeselectCard(pdrv);\n      return false;\n    }\n    const char token = sdWriteBytes(pdrv, currentBuffer, 0xFC);\n    if (token != 0x05) {\n      accepted = false;\n      rejectedToken = token;\n      break;\n    }\n    currentBuffer += 512;\n  } while (--currentCount);\n\n  // The final accepted block may still be programming. Only issue the\n  // multi-write STOP token after it is ready. Never raise CS while busy.\n  if (!sdWait(pdrv, 5000)) {\n    log_e(\"CMD25 pre-stop busy timeout\");\n    sdDeselectCard(pdrv);\n    return false;\n  }\n  sdStop(pdrv);\n  if (!sdWait(pdrv, 5000)) {\n    log_e(\"CMD25 stop-token busy timeout\");\n    sdDeselectCard(pdrv);\n    return false;\n  }\n  sdDeselectCard(pdrv);\n\n  if (!accepted || currentCount != 0) {\n    log_e(\"CMD25 write rejected, data token 0x%02x\", (unsigned int)(uint8_t)rejectedToken);\n    return false;\n  }\n  unsigned int resp = 0;\n  if (sdTransaction(pdrv, SEND_STATUS, 0, &resp) || resp) {\n    return false;\n  }\n  return true;\n}";

function patch(source) {
  let output = source;
  if (!output.includes('SYNAP_SD_CMD24_BUSY_FIX')) {
    if (output.split(before).length !== 2) {
      throw new Error('Pinned Arduino SD CMD24 patch no longer matches ESP32 core 3.3.5');
    }
    output = output.replace(before, after);
  }
  if (!output.includes('SYNAP_SD_CMD25_BUSY_FIX')) {
    if (output.split(multiBefore).length !== 2) {
      throw new Error('Pinned Arduino SD CMD25 patch no longer matches ESP32 core 3.3.5');
    }
    output = output.replace(multiBefore, multiAfter);
  }
  return output;
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
  console.log('Applied pinned C3 CMD24/CMD25 write-completion fixes');
}

module.exports = { patch, install, before, after, multiBefore, multiAfter };
