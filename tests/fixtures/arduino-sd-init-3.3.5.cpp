// From espressif/arduino-esp32 tag 3.3.5, libraries/SD/src/sd_diskio.cpp.
// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD.
// SPDX-License-Identifier: LGPL-2.1-or-later
// Exact upstream initialization function used for behavioral regression tests.
DSTATUS ff_sd_initialize(uint8_t pdrv) {
  char token;
  unsigned int resp;
  unsigned int start;

  // Get the card structure for the given drive number
  ardu_sdcard_t *card = s_cards[pdrv];

  // If the card is already initialized, return its current status
  if (!(card->status & STA_NOINIT)) {
    return card->status;
  }

  // Lock the SPI bus and set it to a low frequency (400kHz) for initialization
  // Low frequency is required during initialization for reliable communication
  AcquireSPI card_locked(card, 400000);

  // Step 1: Power-up sequence - Send at least 74 clock cycles with CS high and MOSI high
  // This is required by the SD card specification to ensure proper card state reset
  // We send 20 bytes (160 clock cycles) to exceed the minimum requirement
  digitalWrite(card->ssPin, HIGH);
  for (uint8_t i = 0; i < 20; i++) {
    card->spi->transfer(0XFF);
  }

  // Step 2: Select the card and send GO_IDLE_STATE command
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
  if (token == 0x5) {
    // Old card that doesn't support CRC - disable CRC checking
    card->supports_crc = false;
  } else if (token != 1) {
    log_w("CRC_ON_OFF failed: %u", token);
    goto unknown_card;
  }

  // Step 4: Card type detection and initialization
  // Try to identify SDHC/SDXC cards using SEND_IF_COND command
  if (sdTransaction(pdrv, SEND_IF_COND, 0x1AA, &resp) == 1) {
    // Card responded to SEND_IF_COND - likely SDHC/SDXC
    if ((resp & 0xFFF) != 0x1AA) {
      log_w("SEND_IF_COND failed: %03X", resp & 0xFFF);
      goto unknown_card;
    }

    // Read Operating Conditions Register to check card capabilities
    if (sdTransaction(pdrv, READ_OCR, 0, &resp) != 1 || !(resp & (1 << 20))) {
      log_w("READ_OCR failed: %X", resp);
      goto unknown_card;
    }

    // Send APP_OP_COND to set operating conditions for SDHC/SDXC
    // Wait up to 1 second for the card to become ready
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0x40100000, NULL);
    } while (token == 1 && (millis() - start) < 1000);

    if (token) {
      log_w("APP_OP_COND failed: %u", token);
      goto unknown_card;
    }

    // Determine if it's SDHC (high capacity) or regular SD
    if (!sdTransaction(pdrv, READ_OCR, 0, &resp)) {
      if (resp & (1 << 30)) {
        card->type = CARD_SDHC;  // High capacity card (SDHC/SDXC)
      } else {
        card->type = CARD_SD;  // Standard capacity card
      }
    } else {
      log_w("READ_OCR failed: %X", resp);
      goto unknown_card;
    }
  } else {
    // Card didn't respond to SEND_IF_COND - try SD or MMC initialization
    if (sdTransaction(pdrv, READ_OCR, 0, &resp) != 1 || !(resp & (1 << 20))) {
      log_w("READ_OCR failed: %X", resp);
      goto unknown_card;
    }

    // Try SD card initialization first
    start = millis();
    do {
      token = sdTransaction(pdrv, APP_OP_COND, 0x100000, NULL);
    } while (token == 0x01 && (millis() - start) < 1000);

    if (!token) {
      card->type = CARD_SD;  // Standard SD card
    } else {
      // Try MMC card initialization
      start = millis();
      do {
        token = sdTransaction(pdrv, SEND_OP_COND, 0x100000, NULL);
      } while (token != 0x00 && (millis() - start) < 1000);

      if (token == 0x00) {
        card->type = CARD_MMC;  // MMC card
      } else {
        log_w("SEND_OP_COND failed: %u", token);
        goto unknown_card;
      }
    }
  }

  // Step 5: Clear card detection for SD cards (not needed for MMC)
  if (card->type != CARD_MMC) {
    if (sdTransaction(pdrv, APP_CLR_CARD_DETECT, 0, NULL)) {
      log_w("APP_CLR_CARD_DETECT failed");
      goto unknown_card;
    }
  }

  // Step 6: Set block length for non-SDHC cards
  // SDHC cards have fixed 512-byte blocks, others need explicit block length setting
  if (card->type != CARD_SDHC) {
    if (sdTransaction(pdrv, SET_BLOCKLEN, 512, NULL) != 0x00) {
      log_w("SET_BLOCKLEN failed");
      goto unknown_card;
    }
  }

  // Step 7: Get card capacity and finalize initialization
  card->sectors = sdGetSectorsCount(pdrv);

  // Limit frequency to 25MHz for compatibility (SD spec maximum for non-UHS cards)
  if (card->frequency > 25000000) {
    card->frequency = 25000000;
  }

  // Mark card as initialized
  card->status &= ~STA_NOINIT;
  return card->status;

unknown_card:
  // Mark card as unknown type if initialization failed
  card->type = CARD_UNKNOWN;
  return card->status;
}

