# Chakshu SD quality review

The SD recorder previously called `configure(true)`, selecting the same 320×240 JPEG quality 22 settings as a Bluetooth preview. A local card therefore did not deliver better detail than a radio preview. Merely raising the sensor resolution would also leave framebuffers allocated for the smaller boot resolution.

The SD path now allocates the camera at the selected resolution, uses JPEG quality 12, and uses two PSRAM framebuffers with `CAMERA_GRAB_LATEST`. Phone photos/previews retain one framebuffer and their existing VGA/QVGA modes. Camera reinitialization happens only on SD start/finish; no voice model or additional boot work is introduced.

| Profile | Resolution | Cadence target | Purpose |
| --- | --- | --- | --- |
| 0 — HD detail | 1280×720 | 10 fps | Stationary scenes and more detail |
| 1 — Smooth | 640×480 | 20 fps | More frequent motion samples |

Targets are not measured guarantees. Exposure, sensor, SD latency and PSRAM bandwidth affect delivery. The writer retains source frame timestamps and drops visual frames instead of blocking the independent PCM acquisition task. JPEG queue slots are bounded at 256 KiB each (two slots); audio has a separate two-second queue. Oversized visual frames count as dropped. A slow audio writer fails visibly and keeps partial files. Existing SD SPI clock/fallbacks are unchanged.

Media descriptor byte 16 adds bit `0x10` for these profiles. In operation 5, offset bits 0–7 select profile 0 or 1; the remaining bits encode clip seconds (15, 30 or 60; zero retains the legacy 60-second default). Invalid options are rejected before recording starts. Operation 6 stops and drains. Operation 9 includes width, height, targetFps, videoProfile and clipLimitMs, plus recorded frames/audioMs/droppedFrames. The app computes actual average FPS from saved frames and soundtrack duration.

Recording stops at its PCM duration limit or 32 MiB of visual data. This preserves the mobile importer’s existing limit. The SD result remains matching MJPEG, mono 16 kHz WAV and JSON timeline files. The sidecar includes resolution, target cadence and dropped frames. It is not hardware H.264 or MP4. A Bluetooth disconnect does not erase or interrupt the accepted local take; power loss can still interrupt file finalization.

Native tests cover camera allocation/restoration, unknown profiles, short clips, separate audio/video rates, stop drain, PCM integrity, queue pressure, card failures, memory/task creation failures and file size limits. CI compiles all three boards. Real-card acceptance still requires checking HD and Smooth in daylight and indoor light, actual FPS/dropped frames, audio sync, return to phone capture, and a Bluetooth disconnect during a timed clip. No measured image-quality improvement is claimed without those hardware checks.

References: [Espressif JPEG/framebuffer guidance](https://github.com/espressif/esp32-camera), [Seeed camera pinout and usage](https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/). Meta’s [capture mode design](https://www.meta.com/blog/ray-ban-meta-gen-2-now-available-ai-glasses-extended-battery-life-3k-video/) informs timed, explicit capture controls; its hardware capabilities are not assumed for Chakshu.
