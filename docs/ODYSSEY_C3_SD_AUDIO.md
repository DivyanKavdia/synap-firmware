# Odyssey C3: local SD audio

C3 retains a successfully mounted SD card at boot. It retries detection on every
local recording start, so a boot-time mount failure or later card insertion does
not require rebooting. No card is formatted and existing files are never replaced.
An SPI adapter without a usable card/filesystem cannot be reported as ready.

| Control/state | Result |
| --- | --- |
| Double tap, BLE disconnected | Start 16 kHz mono PCM16 WAV on SD |
| Double tap, BLE connected, no local take | Existing PWA audio start/stop path |
| Double tap during an SD take | Finalize and close that SD take |
| BLE connects during an SD take | Continue saving the same take to SD; double tap stops it |
| Four-second hold during an SD take | Finalize the take, then enter sleep |
| Missing/unwritable card | Log failure; retain normal BLE operation; retry on next offline start |

Files are named `/synap/odyssey_audio_<random>_<random>.wav`. The header is
checkpointed every two seconds and finalized on stop. A write failure closes the
file and marks SD unavailable; already written data is retained where the card
still permits finalization. Abrupt power loss or removal can still damage FAT data.
Recording ends at the WAV length limit; it never deletes older recordings.

SD pins remain CS GPIO0, SCK GPIO10, MOSI GPIO21, MISO GPIO20. Touch remains
GPIO3; microphone GPIO4/5/6; NeoPixel GPIO8; battery ADC GPIO1. USB CDC must be
enabled to keep UART0 off the SD pins. A dim green pulse indicates local activity.

Local capture blocks OTA, idle sleep and competing microphone use. It does not
change the connected BLE audio/recovery protocol or the S3/Chakshu recording
implementation. This change adds local WAV storage only: C3 SD catalogue/transfer
to the PWA is not implemented, and SD media-transfer capabilities are not advertised.
Read the WAV files from the card for now.

Validation: native recorder tests cover PCM conversion with partial reads, WAV
lengths/checkpoints, stop, reconnect, filename collisions, unavailable card, retry,
short writes, microphone/open/task failures and busy guards. Touch tests cover all
three targets. Hardware acceptance still requires boot/mount, offline capture and
playback, reconnect during capture, connected PWA capture, and card failure tests
on a physical C3 with this wiring.
