// From espressif/arduino-esp32 tag 3.3.5, libraries/SD/src/sd_diskio.cpp.
// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD.
// SPDX-License-Identifier: LGPL-2.1-or-later
bool sdWriteSector(uint8_t pdrv, const char *buffer, unsigned long long sector) {
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
}

