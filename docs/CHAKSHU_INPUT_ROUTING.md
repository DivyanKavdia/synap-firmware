# Chakshu input routing

This document is the production contract for how Chakshu routes actions by **initiator**. BLE connectivity does not choose the storage destination.

## Routing matrix

| Initiator | BLE connected | Action | Destination |
| --- | --- | --- | --- |
| Hey Snap | Yes | Audio / photo / video / describe | Chakshu SD |
| Hey Snap | No | Audio / photo / video / describe | Chakshu SD |
| PWA | Yes | Audio / photo / video | Live BLE transport to PWA / phone storage |
| TTP223 double tap | Yes | Audio start / stop | Live BLE transport to PWA / phone storage |
| TTP223 double tap | No | Audio start / stop | Chakshu SD |
| TTP223 | Any | Photo / video | **Not supported** |

## Voice contract

1. The TinyML wake engine remains armed while Chakshu is connected or disconnected.
2. "Hey Snap" arms the existing bounded next-command gate. A media command must follow the wake phrase.
3. Voice-originated media always uses the local SD capture path.
4. A spoken Stop controls only the voice-owned SD session. It never stops a PWA/TTP live recording.
5. When BLE is connected, voice state and completion are surfaced to the PWA by the dedicated voice event characteristic; the PWA does not poll voice status.
6. Legacy VOICE_OFF ownership writes are compatibility no-ops. BLE connection is no longer a voice ownership transfer.

## Concurrency and safety

- Camera, microphone/streaming, SD media and OTA continue to use the existing resource-admission gates.
- A Hey Snap capture does not start over an active PWA live stream, OTA operation or conflicting media job. It reports busy instead.
- BLE clients cannot invoke the local SD audio/video recorder directly. This preserves the rule that PWA-started capture is stored on the phone.
- Connecting or disconnecting BLE does not invalidate a queued local voice request merely because link ownership changed.
- Existing SD lifecycle rules remain unchanged: captures stay on SD until a verified sync/import, after which the PWA may delete the verified source file.

## TTP223 contract

The touch gesture remains intentionally narrow:

- double tap while connected: start/stop the live audio stream for the PWA;
- double tap while disconnected: start/stop SD audio capture;
- four-second hold: existing sleep/wake behavior;
- no tap gesture captures an image or starts video.

This separation keeps the physical control deterministic and prevents accidental camera activation.
