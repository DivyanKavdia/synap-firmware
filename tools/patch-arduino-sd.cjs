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
      synapRecordSdWriteFault(24,1,0,0);
      return false;
    }
    if (!sdCommand(pdrv, WRITE_BLOCK_SINGLE, (s_cards[pdrv]->type == CARD_SDHC) ? sector : sector << 9, NULL)) {
      char token = sdWriteBytes(pdrv, buffer, 0xFE);

      // SYNAP_SD_CMD24_BUSY_FIX:
      // 0x05 is the only accepted SD data-response token. Keep CS asserted
      // while the card programs the block, matching ESP-IDF SDSPI semantics.
      if (token != 0x05) {
        synapRecordSdWriteFault(24,3,0,uint8_t(token));
        sdDeselectCard(pdrv);
        // CRC error is retryable; write/unknown errors are not.
        if (token == 0x0B) {
          continue;
        }
        return false;
      }
      if (!sdWait(pdrv, 5000)) {
        synapRecordSdWriteFault(24,4,0,0);
        sdDeselectCard(pdrv);
        return false;
      }
      sdDeselectCard(pdrv);

      unsigned int resp;
      const char status=sdTransaction(pdrv, SEND_STATUS, 0, &resp);
      if (status || resp) {
        synapRecordSdWriteFault(24,5,0,status?uint8_t(status):uint8_t(resp));
        return false;
      }
      return true;
    } else {
      synapRecordSdWriteFault(24,2,0,0);
      break;
    }
  }
  sdDeselectCard(pdrv);
  return false;
}`;

// Compact pinned driver fault code: [command 24/25][phase][sector index][token].
// SD cards may terminate CMD25 after one block. Use that same completion
// path for FAT/directory sectors; preserve the original MMC command choice.
const singleAfter = `bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count);
${after.replace('bool sdWriteSector(', 'bool synapSdWriteMmcSector(')}
bool sdWriteSector(uint8_t pdrv, const char *buffer, unsigned long long sector) {
  // SYNAP_SD_METADATA_CMD25: exactly one sector, then write STOP and status.
  // Never fall back to CMD24 after a timeout: durability is then unknown.
  if (s_cards[pdrv]->type == CARD_MMC)
    return synapSdWriteMmcSector(pdrv, buffer, sector);
  return sdWriteSectors(pdrv, buffer, sector, 1);
}`;

// Capture the FIRST error in a recording, even if fclose subsequently writes.
const faultHeader = `static volatile uint32_t synapSdWriteFault=0;
static inline void synapRecordSdWriteFault(uint8_t command,uint8_t phase,uint8_t block,uint8_t token) {
  if (!synapSdWriteFault)
    synapSdWriteFault=(uint32_t(command)<<24)|(uint32_t(phase)<<16)|(uint32_t(block)<<8)|token;
}
extern "C" uint32_t synapSdWriteFaultCode() { return synapSdWriteFault; }
extern "C" void synapSdClearWriteFaultCode() { synapSdWriteFault=0; }
`;

// A second 500 ms wait inside sdWriteBytes undermined the CMD25 5s timeout.
// Only the C3 driver is patched; tolerate slow internal SD erase/program.
const byteBefore = `char sdWriteBytes(uint8_t pdrv, const char *buffer, char token) {
  ardu_sdcard_t *card = s_cards[pdrv];
  unsigned short crc = (card->supports_crc) ? CRC16(buffer, 512) : 0xFFFF;
  if (!sdWait(pdrv, 500)) {
    return 0;
  }

  card->spi->write(token);
  card->spi->writeBytes((uint8_t *)buffer, 512);
  card->spi->write16(crc);
  return (card->spi->transfer(0xFF) & 0x1F);
}`;
const byteTimeoutOnly = byteBefore.replace('sdWait(pdrv, 500)', 'sdWait(pdrv, 5000)');
const byteAfter = byteTimeoutOnly.replace(
  'return (card->spi->transfer(0xFF) & 0x1F);',
  `// SYNAP_SD_WRITE_BUSY_LATENCY: the response-to-busy interval is not
  // programming completion. Clock its one-byte allowance before callers
  // test ready; otherwise 0xFF,0x00,... can prematurely release CS or send
  // the next CMD25 data token while the card is still programming.
  const char response = card->spi->transfer(0xFF) & 0x1F;
  card->spi->transfer(0xFF);
  return response;`);

const stopBefore = `void sdStop(uint8_t pdrv) {
  s_cards[pdrv]->spi->write(0xFD);
}`;
const stopAfter = `void sdStop(uint8_t pdrv) {
  s_cards[pdrv]->spi->write(0xFD);
  // SYNAP_SD_STOP_BUSY_LATENCY: match ESP-IDF's {0xFD, 0xFF} stop
  // transaction before polling busy. The first byte may still be idle.
  s_cards[pdrv]->spi->transfer(0xFF);
}`;

// Exact pinned Arduino-ESP32 3.3.5 CMD25 source. Fail closed on core drift.
const multiBefore = "bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count) {\n  char token;\n  const char *currentBuffer = buffer;\n  unsigned long long currentSector = sector;\n  int currentCount = count;\n  ardu_sdcard_t *card = s_cards[pdrv];\n\n  for (int f = 0; f < 3;) {\n    if (card->type != CARD_MMC) {\n      if (sdTransaction(pdrv, SET_WR_BLK_ERASE_COUNT, currentCount, NULL)) {\n        return false;\n      }\n    }\n\n    if (!sdSelectCard(pdrv)) {\n      return false;\n    }\n\n    if (!sdCommand(pdrv, WRITE_BLOCK_MULTIPLE, (card->type == CARD_SDHC) ? currentSector : currentSector << 9, NULL)) {\n      do {\n        token = sdWriteBytes(pdrv, currentBuffer, 0xFC);\n        if (token != 0x05) {\n          f++;\n          break;\n        }\n        currentBuffer += 512;\n        f = 0;\n      } while (--currentCount);\n\n      if (!sdWait(pdrv, 500)) {\n        break;\n      }\n\n      if (currentCount == 0) {\n        sdStop(pdrv);\n        sdDeselectCard(pdrv);\n\n        unsigned int resp;\n        if (sdTransaction(pdrv, SEND_STATUS, 0, &resp) || resp) {\n          return false;\n        }\n        return true;\n      } else {\n        if (sdCommand(pdrv, STOP_TRANSMISSION, 0, NULL)) {\n          break;\n        }\n\n        if (token == 0x0A) {\n          sdDeselectCard(pdrv);\n          unsigned int writtenBlocks = 0;\n          if (card->type != CARD_MMC && sdSelectCard(pdrv)) {\n            if (!sdCommand(pdrv, SEND_NUM_WR_BLOCKS, 0, NULL)) {\n              char acmdData[4];\n              if (sdReadBytes(pdrv, acmdData, 4)) {\n                writtenBlocks = acmdData[0] << 24;\n                writtenBlocks |= acmdData[1] << 16;\n                writtenBlocks |= acmdData[2] << 8;\n                writtenBlocks |= acmdData[3];\n              }\n            }\n            sdDeselectCard(pdrv);\n          }\n          currentBuffer = buffer + (writtenBlocks << 9);\n          currentSector = sector + writtenBlocks;\n          currentCount = count - writtenBlocks;\n          continue;\n        } else {\n          break;\n        }\n      }\n    } else {\n      break;\n    }\n  }\n  sdDeselectCard(pdrv);\n  return false;\n}";
const multiAfter = "bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count) {\n  const char *currentBuffer = buffer;\n  unsigned long long currentSector = sector;\n  int currentCount = count;\n  ardu_sdcard_t *card = s_cards[pdrv];\n  if (count <= 1) {\n    synapRecordSdWriteFault(25,9,0,0);\n    return false;\n  }\n\n  if (card->type != CARD_MMC) {\n    if (sdTransaction(pdrv, SET_WR_BLK_ERASE_COUNT, currentCount, NULL)) {\n      synapRecordSdWriteFault(25,1,0,0);\n      return false;\n    }\n  }\n\n  if (!sdSelectCard(pdrv)) {\n    synapRecordSdWriteFault(25,2,0,0);\n    return false;\n  }\n  if (sdCommand(pdrv, WRITE_BLOCK_MULTIPLE, (card->type == CARD_SDHC) ? currentSector : currentSector << 9, NULL)) {\n    synapRecordSdWriteFault(25,3,0,0);\n    sdDeselectCard(pdrv);\n    return false;\n  }\n\n  // SYNAP_SD_CMD25_BUSY_FIX: CMD25 accepts 0xFC data blocks and terminates\n  // with 0xFD, never a CMD12 read-stop command. Keep CS LOW until the stop\n  // token has finished its internal flash programming (bounded to 5 seconds).\n  // Do not blindly retry a partially written FAT sector range.\n  bool accepted = true;\n  char rejectedToken = 0x05;\n  do {\n    // Stock Arduino only waits 500 ms before a block; a card busy with flash\n    // erase/program is permitted more time, without changing SPI frequency.\n    if (!sdWait(pdrv, 5000)) {\n      synapRecordSdWriteFault(25,4,uint8_t(count-currentCount),0);\n      log_e(\"CMD25 block-ready timeout\");\n      sdDeselectCard(pdrv);\n      return false;\n    }\n    const char token = sdWriteBytes(pdrv, currentBuffer, 0xFC);\n    if (token != 0x05) {\n      accepted = false;\n      rejectedToken = token;\n      synapRecordSdWriteFault(25,5,uint8_t(count-currentCount),uint8_t(token));\n      break;\n    }\n    currentBuffer += 512;\n  } while (--currentCount);\n\n  // The final accepted block may still be programming. Only issue the\n  // multi-write STOP token after it is ready. Never raise CS while busy.\n  if (!sdWait(pdrv, 5000)) {\n    synapRecordSdWriteFault(25,6,uint8_t(count-currentCount),0);\n    log_e(\"CMD25 pre-stop busy timeout\");\n    sdDeselectCard(pdrv);\n    return false;\n  }\n  sdStop(pdrv);\n  if (!sdWait(pdrv, 5000)) {\n    synapRecordSdWriteFault(25,7,uint8_t(count-currentCount),0);\n    log_e(\"CMD25 stop-token busy timeout\");\n    sdDeselectCard(pdrv);\n    return false;\n  }\n  sdDeselectCard(pdrv);\n\n  if (!accepted || currentCount != 0) {\n    log_e(\"CMD25 write rejected, data token 0x%02x\", (unsigned int)(uint8_t)rejectedToken);\n    return false;\n  }\n  unsigned int resp = 0;\n  const char status=sdTransaction(pdrv, SEND_STATUS, 0, &resp);\n  if (status || resp) {\n    synapRecordSdWriteFault(25,8,0,status?uint8_t(status):uint8_t(resp));\n    return false;\n  }\n  return true;\n}";

// ESP-IDF v5.5 SDSPI poll_busy requires two nonzero observations while CS
// remains asserted. Arduino-ESP32 3.3.5 sdWait() returns on the first
// nonzero byte, which may be a transient idle byte before busy begins.
// Preserve the existing 5s deadline and do not retry uncertain writes.
const stableBusyPoll = `// SYNAP_SD_STABLE_BUSY_POLL: C3 CMD25 data and STOP programming only.
static bool synapSdWaitStable(uint8_t pdrv, int timeoutMs) {
  const uint32_t started = millis();
  uint8_t nonzero = 0;
  do {
    const uint8_t value = s_cards[pdrv]->spi->transfer(0xFF);
    if (value != 0 && ++nonzero >= 2) return true;
  } while (uint32_t(millis() - started) < uint32_t(timeoutMs));
  return false;
}`;
const canonicalMultiAfter = multiAfter.replace('if (count <= 1) {','if (count <= 0) {');
const stableMultiAfter = canonicalMultiAfter.replaceAll(
  'sdWait(pdrv, 5000)', 'synapSdWaitStable(pdrv, 5000)');
if (stableMultiAfter === canonicalMultiAfter) throw new Error('Expected CMD25 ready polls');

// IDF-compatible ready polling is deliberately *not* applied to CMD24/MMC,
// mount, read or SPI card selection.

function patchOld(source) {
  let output = source;
  if (output.includes('void sdStop(uint8_t pdrv)') && !output.includes(stopAfter)) {
    if (output.split(stopBefore).length!==2)
      throw new Error('Pinned Arduino SD stop token changed from 3.3.5');
    output=output.replace(stopBefore,stopAfter);
  }
  if (output.includes('char sdWriteBytes(uint8_t pdrv') &&
      !output.includes(byteAfter)) {
    const previous=output.includes(byteTimeoutOnly)?byteTimeoutOnly:byteBefore;
    if (output.split(previous).length!==2)
      throw new Error('Pinned Arduino SD data block busy timeout changed from 3.3.5');
    output=output.replace(previous,byteAfter);
  }
  if (!output.includes('SYNAP_SD_CMD24_BUSY_FIX')) {
    if (output.split(before).length !== 2) {
      throw new Error('Pinned Arduino SD CMD24 patch no longer matches ESP32 core 3.3.5');
    }
    output = output.replace(before, faultHeader + after);
  }
  if (output.includes('SYNAP_SD_CMD24_BUSY_FIX') &&
      !output.includes('extern "C" uint32_t synapSdWriteFaultCode()')) {
    const anchor='bool sdWriteSector(uint8_t pdrv,';
    if (output.split(anchor).length !== 2) throw new Error('C3 SD trace insertion ambiguous');
    output=output.replace(anchor,faultHeader+anchor);
  }
  if (!output.includes('SYNAP_SD_CMD25_BUSY_FIX')) {
    if (output.split(multiBefore).length !== 2) {
      throw new Error('Pinned Arduino SD CMD25 patch no longer matches ESP32 core 3.3.5');
    }
    output = output.replace(multiBefore, multiAfter);
  }
  if (!output.includes('SYNAP_SD_METADATA_CMD25')) {
    if (output.split(after).length!==2)
      throw new Error('Pinned C3 single-sector write path changed');
    output=output.replace(after,singleAfter);
  }
  // Upgrade the previous C3 patch too, without duplicating its implementation.
  const oldCount='if (count <= 1) {\n    synapRecordSdWriteFault(25,9,0,0);';
  const newCount='if (count <= 0) {\n    synapRecordSdWriteFault(25,9,0,0);';
  if (output.includes(oldCount)) output=output.replace(oldCount,newCount);
  else if (!output.includes(newCount)) throw new Error('Pinned C3 CMD25 count guard changed');
  if (!output.includes('SYNAP_SD_STABLE_BUSY_POLL')) {
    if (output.split(canonicalMultiAfter).length !== 2)
      throw new Error('Pinned C3 CMD25 stable-busy upgrade source drift');
    output=output.replace(canonicalMultiAfter,stableBusyPoll+String.fromCharCode(10)+stableMultiAfter);
  }
  return output;
}


const c3Single = singleAfter
  .replace('SYNAP_SD_METADATA_CMD25: exactly one sector, then write STOP and status.',
           'SYNAP_SD_C3_CMD24_ONLY: verified CMD24 for every FAT and audio sector.')
  .replace('Never fall back to CMD24 after a timeout: durability is then unknown.',
           'Never replay a sector with uncertain completion.')
  .replace('if (s_cards[pdrv]->type == CARD_MMC)\\n    return synapSdWriteMmcSector(pdrv, buffer, sector);\\n  return sdWriteSectors(pdrv, buffer, sector, 1);',
           'return synapSdWriteMmcSector(pdrv, buffer, sector);')
  .replace('bool synapSdWriteMmcSector(uint8_t pdrv,',
           'static bool synapSdWaitStable(uint8_t pdrv, int timeoutMs);\\nbool synapSdWriteMmcSector(uint8_t pdrv,')
  .replace('if (!sdWait(pdrv, 5000)) {','if (!synapSdWaitStable(pdrv, 5000)) {')
  .replace('unsigned int resp;','unsigned int resp=0;')
  .replace('if (status || resp) {\\n        synapRecordSdWriteFault(24,5,0,status?uint8_t(status):uint8_t(resp));\\n        return false;\\n      }',
           'if (status) {\\n        synapRecordSdWriteFault(24,5,0,uint8_t(status));\\n        return false;\\n      }\\n      if (resp) {\\n        synapRecordSdWriteFault(24,6,0,uint8_t(resp));\\n        return false;\\n      }');
const c3Ready = stableBusyPoll
  .replace('SYNAP_SD_STABLE_BUSY_POLL: C3 CMD25 data and STOP programming only.',
           'SYNAP_SD_C3_STABLE_READY: CMD24 block programming, keep CS low.')
  .replace('uint8_t nonzero = 0;', 'uint8_t idle = 0;')
  .replace('if (value != 0 && ++nonzero >= 2) return true;',
           'if (value == 0xFF) { if (++idle >= 16) return true; } else idle = 0;');
const c3Multi = `bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count) {
  // SYNAP_SD_C3_CMD24_SECTORS: bounded CMD24 per sector, no CMD25/STOP.
  if (count<=0) {
    synapRecordSdWriteFault(24,9,0,0);
    return false;
  }
  for (int index=0;index<count;++index) {
    // SD sector may already be programmed on failure: do not retry.
    if (!sdWriteSector(pdrv,buffer+size_t(index)*512u,sector+unsigned(index)))
      return false;
  }
  return true;
}`;

// This wrapper leaves Arduino 3.3.5 driver matching and historic upgrades
// unchanged, then selects the C3-only CMD24 path. No S3 target uses it.
function patch(source) {
  if (source.includes('SYNAP_SD_C3_CMD24_SECTORS')) return source;
  let output=patchOld(source);
  if (!c3Single.includes('synapSdWaitStable(pdrv, 5000)') ||
      !c3Single.includes('SYNAP_SD_C3_CMD24_ONLY') ||
      !c3Single.includes('synapRecordSdWriteFault(24,6') ||
      !c3Ready.includes('idle >= 16'))
    throw new Error('C3 CMD24 driver source rewrite mismatch');
  if (output.split(singleAfter).length!==2 ||
      output.split(stableBusyPoll+'\\n'+stableMultiAfter).length!==2)
    throw new Error('C3 CMD24 conversion source drift');
  output=output.replace(singleAfter,c3Single);
  output=output.replace(stableBusyPoll+'\\n'+stableMultiAfter,c3Ready+'\\n'+c3Multi);
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
  console.log('Applied C3-only CMD24 verified single-sector writes');
}

module.exports = { patch, patchOld, c3Single, c3Multi, c3Ready, install, before, after, singleAfter, multiBefore, multiAfter, faultHeader, byteBefore, byteAfter, byteTimeoutOnly, stopBefore, stopAfter };
