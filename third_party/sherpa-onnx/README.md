# sherpa-onnx C API header

`c-api.h` from sherpa-onnx v1.13.8 (https://github.com/k2-fsa/sherpa-onnx),
Apache License 2.0 (see LICENSE). OpenTIE-VR only uses its types: the library
itself (`libsherpa-onnx-c-api.so` + `libonnxruntime.so`, MIT) is loaded at
run time for VR voice commands and ships in the Quest APK.

Speech model: `sherpa-onnx-zipformer-small-en-2023-06-26` (int8, non-streaming),
from the sherpa-onnx `asr-models` release, Apache License 2.0.

## Packaging (Quest APK)

- `lib/arm64-v8a/libsherpa-onnx-c-api.so`, `lib/arm64-v8a/libonnxruntime.so`:
  from `sherpa-onnx-v1.13.8-android.tar.bz2`
  (https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.8, `jniLibs/arm64-v8a`).
- `assets/voice/{encoder,decoder,joiner}.onnx`, `assets/voice/tokens.txt`:
  the `*.int8.onnx` files and `tokens.txt` of
  `sherpa-onnx-zipformer-small-en-2023-06-26.tar.bz2`
  (https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models), renamed.
- The manifest needs `android.permission.RECORD_AUDIO`
  (`packaging/quest/tools/package_tie_quest.sh` adds it and the model, from `$VOICE`).

The game (src/tie_app/quest/vr_voice.c) uses a `voice-en` folder with these four files next to the game
data (`Android/data/<package>/files/voice-en`) when there is one; else it
copies the model out of the APK to its internal storage on first use
(about 28 MB). Leaving the model out of the APK keeps it small.
