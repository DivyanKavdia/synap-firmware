# Chakshu voice training continuity

Production remains the experimental eight-class build-1400 model until a reviewed candidate is explicitly promoted. Runtime order is fixed: noise, unknown, Hey Snap, photo, video, stop, audio, describe. Never regenerate a six-class header: it drops the audio/describe routes.

## Reproducible candidate workflow

The offline trainer requires Python with numpy, scipy, scikit-learn and torch, plus an `espeak` executable. Training is not a firmware-build dependency.

```sh
python3 tools/test-train-tiny-voice.py
python3 tools/train-tiny-voice.py --count 220 --epochs 180
# With annotated real recordings:
python3 tools/train-tiny-voice.py --real-manifest /private/data/utterances.json --out build/voice-candidate/personalized.h --report build/voice-candidate/personalized.json
```

Default outputs are `build/voice-candidate/tiny-voice-model.h` and `build/voice-candidate/report.json`. Candidate generation does not overwrite the production header. The report contains quantized synthetic accuracy and confusion matrix, real session-holdout results when available, original real-utterance counts, and an explicit experimental status. The 85% synthetic export gate is only a bootstrap check; it does not establish real-world quality.

Synthetic train/test groups keep the same source synthesis parameters together. This is still a synthetic test, not an independent speaker test. Real augmentation happens only after partitioning original annotated utterances. Test utterances use deterministic centre windows without augmentation. Audio longer than the 1.5-second runtime window is centre-cropped for this isolated-utterance metric; separately validate streaming recognition before release.

## Real recording manifest

Use a private JSON array with one manually checked utterance interval per row:

```json
[
  {"path":"session-1.wav","label":"audio","start":1.2,"end":2.5,"session":"speaker1-session1","split":"train"},
  {"path":"session-2.wav","label":"audio","start":0.5,"end":1.9,"session":"speaker1-session2","split":"test"}
]
```

Paths resolve relative to the manifest. Inputs must be 16 kHz mono PCM16 WAV. Keep every recording session wholly in one split. The loader rejects duplicate utterances, invalid intervals, shared sessions, and reuse of the same recording bytes across splits even under renamed files. The caller must assign truthful session identities; software cannot infer when audio was recorded. Keep voice files and private manifests out of the public repository.

The three recovered user files are Record audio, Record video, and Explain what you see. Build 1400 documents 22 selected utterances but does not retain their exact segmentation or original training run. Do not claim an exact reproduction of that run. Splitting repetitions from one recording is not an independent session holdout. Collect separate sessions for each command, plus Hey Snap, photo, stop, normal conversation, confusable phrases and room noise, before making a production-accuracy claim.

## Promotion and deployment

Inspect per-class confusion and held-out negatives, not just overall accuracy. Compare against the existing quantized model on the same independent data. Check wake-plus-action streaming behavior and false activations as well as isolated phrase accuracy. Preserve the current production model when evidence is insufficient.

After explicitly selecting a candidate, copy its header to `firmware/xiao-sense/tiny-voice-model.h`, record dataset provenance and limitations in its header and this document, run source assembly and firmware contract tests, then use the normal PR/CI/OTA release path. Confirm all three targets compile and the published feed references the intended source. Device installation and offline capture/sync acceptance require the pendant.

Training changes must preserve SD pin ownership, mount/recovery, unsynced-file protection and BLE-exclusive command ownership. Do not change these to compensate for a weak classifier.

## 21 September 2026 continuation result

The corrected eight-class pipeline completed 180 epochs with 220 synthetic examples per class, seed 7 and quantized evaluation: **429/442 (97.06%)** held-out synthetic windows. This run used PyTorch 2.14.0+cpu and eSpeak NG from espeakng-loader 0.2.4 through a local command adapter. Different synthesis versions can change results. Full class confusion is retained in `voice-training-20260921.json`.

No real utterances were included in this candidate run and no independent real accuracy was measured. The recovered recordings remain available, but their earlier segmentation was not recoverable and this pass did not invent annotations. This synthetic-only candidate was **not promoted**; the personalized production weights remain unchanged. Five trainer tests passed (split leakage, copied source recordings, invalid intervals, deterministic evaluation and C++ compilation of the eight-class exported header), along with 13 targeted firmware contracts covering voice, SD and ownership.
