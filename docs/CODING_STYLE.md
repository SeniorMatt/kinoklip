# Coding Style & Conventions

The goal: **a codebase a new human contributor can read top-to-bottom and extend without asking
anyone.** When in doubt, prefer the clearer code over the clever code.

## 1. Formatting (enforced)
- `clang-format` with the repo's `.clang-format` (KDE/Qt-like, 4 spaces, 100 columns).
  Run `scripts/format.sh` before every commit. `.editorconfig` covers other files.
- UTF-8, LF line endings, newline at end of file, no trailing whitespace.
- `#pragma once` in every header.
- Include order (clang-format groups them): own header → project headers (`"core/..."`) → Qt → std.

## 2. Naming
| Thing | Style | Example |
|---|---|---|
| Files | PascalCase, one main class per file | `TimelineWidget.h/.cpp` |
| Classes, structs, enums, type aliases | PascalCase | `ClipData`, `enum class TrackType` |
| Enum values | PascalCase | `TrackType::Video` |
| Functions, methods, variables | camelCase | `renderFrame()`, `clipStart` |
| Private/protected data members | `m_` prefix | `m_playhead` |
| Plain public struct fields | no prefix | `clip.start` |
| Constants (`constexpr`) | `k` prefix | `kDefaultFps` |
| Namespaces | lower case | `cw`, `cw::media`, `cw::ui` |
| Signals | past tense or "...Changed" | `changed()`, `playheadChanged()` |
| Qt slots | imperative | `play()`, `setRate()` |

**Units in names when ambiguous:** `sourceInUs`, `durationFrames`, `gainDb`, `sampleIndex48k`.
Frames vs microseconds confusion is the #1 bug source in NLEs; never write a bare `time`.

## 3. Comments & documentation
- **File header** (every file): SPDX line + 1–3 lines saying what the file is for.
  ```cpp
  // SPDX-License-Identifier: GPL-3.0-or-later
  // TimelineWidget — draws tracks/clips and turns mouse/keyboard input into EditOps.
  ```
- **Every public class** has a Doxygen `///` block: responsibility, ownership, thread-safety.
- **Every public function** in a header has a `///` brief: what it does, units of parameters,
  return value / failure behaviour. Skip only for self-evident getters/setters.
- Implementation comments explain **why**, not what. Use them before non-obvious algorithms
  (seeking, A/V sync, transition maths) with a short description of the approach.
- `// TODO(area): ...` for known gaps — they are collected into the morning report.
- No commented-out code. No ASCII-art banners beyond simple `// ---- Section ----`.

## 4. Module structure & extensibility
- Each `src/<module>/` has a `README.md`: responsibility, key classes, data flow,
  **"How to extend"** (step-by-step for the common extension: new effect, new action, new page,
  new export preset, new panel...).
- Dependencies point one way: `core` ← `media` ← `render` ← `playback`/`export` ← `ui` ← `app`.
  `core` never includes from `ui`/`app`. `media` never includes Qt Widgets.
- Extension points use registries, not `switch` statements scattered through the code:
  `EffectRegistry` (effects/transitions/generators), `ActionRegistry` (menu actions & shortcuts),
  page list in `MainWindow`, `ExportPresets`.
- Prefer small classes (< ~500 lines per .cpp). Split by responsibility when they grow.
- Free functions over classes for pure logic (`EditOps`, interpolation maths) — easier to test.

## 5. C++ rules
- C++20, Qt 6.4 API. No compiler-specific extensions. Everything in `namespace cw`.
- Ownership: Qt parent/child for QObjects; `std::unique_ptr` / `std::shared_ptr` otherwise.
  No raw owning `new`/`delete` outside Qt parenting.
- `const` everything that can be. Pass big values by `const&`. `[[nodiscard]]` on functions whose
  result must be checked.
- Error handling: return `bool` + `QString* error`, or `std::optional`/expected-like struct.
  No exceptions crossing module boundaries. Log with `qCWarning(category)` (one logging category
  per module, e.g. `cw.media`).
- Threads: document it on the class (`/// Thread-safety: ...`). Render/decode threads only touch
  immutable snapshots (`std::shared_ptr<const TimelineData>`).
- No magic numbers: named `constexpr` constants with a comment.
- UI strings wrapped in `tr()` so the app can be translated later.

## 6. Tests & commits
- Logic gets QtTest unit tests in `tests/<module>/test_<thing>.cpp`.
- Commits: `<area>: <imperative summary>` (e.g. `timeline: add ripple trim`), body explains why.
