# src/matting - Background Removal (AI tool, spec 78)

Cuts a subject out of footage or a still picture with a local model (two models: people in video, any subject). Optional: built with ONNX Runtime when found, the model
is downloaded only when the user asks. Logging category: none (quiet unless something fails).

| File | What |
|---|---|
| `ModelCatalog.h` | the two models (`defaultModel()` Robust Video Matting, MobileNetV3; `imageModel()` BiRefNet general Swin-T; float32 ONNX): name, size, SHA-256, official URL, licence; the models folder (`<AppDataLocation>/models`), install / remove, the URL rules, and `installDownloaded()` (size + checksum, then rename) |
| `ModelDownload.h` | the HTTPS download with progress and cancel; used only by the download dialog (`ui/dialogs/AiModelDialog.h`) |
| `MatteBackend.h` | the interface of one model session (`process(rgb, quality, reset)` -> 8-bit matte); `RvmBackend.cpp` (video model) and `BirefnetBackend.cpp` (any subject, per picture) are the ONNX Runtime implementations (compiled out without the runtime); `MatteModel` picks one, `resolveModel()` does Automatic |
| `MatteMath.h` | the picture maths of the any-subject model, runtime-free and tested: input tensor, sigmoid, resize, guided-filter edge refinement |
| `MatteCache.h` | mattes on disk: `<cache folder>/MatteCache/<key>/<ms>.png` |
| `MatteService.h` | the singleton the renderer asks for mattes: memory + disk lookup, a lowest-priority worker, look-ahead, whole-clip ranges, waiting for renders, recurrent-state reset on seeks |

The effect is `core/BuiltinVideoEffects8.cpp` (descriptor) and `render/GlBackgroundRemoval.cpp` (pixels); the
Inspector part is `ui/inspector/InspectorBackgroundRemoval.cpp`; Preferences > System > AI Models is in
`ui/dialogs/AiModelDialog.cpp`.

## How to extend
- Another model: add a `ModelInfo` (id, URL, size, SHA-256, licence evidence in `attribution.md`) and a `MatteBackend`;
  the licence of code and weights must be GPL-3 compatible and open source.
- Tests plug in a stand-in with `MatteService::setBackendFactory()` / `setFrameSource()`; never commit model files.
