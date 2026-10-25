# Odyssey C3+SD compressed BLE sync

Media capability version 4 is compiled only for `esp32c3-supermini-4m-sd`
and `esp32c3-supermini-4m-sd-ota`. Standard C3, S3 and Chakshu retain existing behaviour.

The recorder and SD files remain 16 kHz mono PCM16. Opcode 36 reads the selected
file using its connection-bound selection token and source byte offsets. The
44-byte virtual WAV header is sent unchanged; subsequent 800-sample blocks use
the existing independent-frame IMA ADPCM encoder. Each 426-byte notification
contains the request ID, total source bytes, source offset, valid sample count,
404 encoded bytes and IEEE CRC32 of the preceding 422 bytes. Final padding is
never imported. Six packets per window retain 20 ms pacing. Partial windows
resume at the first missing source offset. Small MTUs fall back to PCM reads.

The phone reconstructs a WAV with the original duration and sample count. IMA
ADPCM is lossy: an imported WAV is not a bit-identical backup of the SD PCM.
Transport CRC and durable import verification do not assert original PCM equality.
The original is retained; deletion/clear quarantine and SD read/write safety are
unchanged. The protocol does not change SD clock, partition tables or Wi-Fi.

Publish with `[c3-sd-only]` in the main commit message to update only the two SD
feeds, with matching provenance and feed checks. All five targets still compile
in CI. PWA shell 235 enables opcode 36 only for module 2 advertising media-v4;
older firmware/PWA pairs remain compatible through existing PCM reads.

Validation: native sender tests cover source offsets, short final frame, repeatable
retries, stale tokens, odd offsets, low MTU, busy capture and read quarantine.
Browser transport tests cover decoding, CRC rejection, missing offsets, final
trimming, module/version gating and PCM fallback. Actual Bluefy throughput,
transcription quality, reconnection and recording after sync require hardware tests.
