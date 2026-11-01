# Coding Style & Conventions

Priorities, in order: **1. performance, 2. correctness, 3. readability.**
When the fastest approach is harder to read, keep the fast approach and **explain it with
comments** (what it does, why it's fast, invariants it relies on). Comments are how we stay
readable without giving up speed — so they are mandatory, not optional.

Performance rules:
- Nothing slow on the GUI thread (decode, render, disk I/O, probing) — use worker threads.
- Hot paths (per frame / per sample) are allocation-free in steady state: reuse buffers, pools,
  `reserve()`, no `QString` formatting, no `QVariant`, no virtual calls per pixel.
- Pixels are processed on the GPU (OpenGL 3.3 core shaders); CPU fallbacks only where needed.
- Measure before and after optimising; leave the benchmark numbers in a comment or test output.

## 0. Base style: KDE Frameworks coding style
We follow the KDE coding style (https://community.kde.org/Policies/Frameworks_Coding_Style),
which builds on the Qt style. Where this document is silent, KDE's policy applies. Key points:
- 4 spaces, no tabs; column limit 160 (KDE clang-format default).
- Braces: opening brace on its own line for classes, structs and functions; on the same line
  for control statements and namespaces. **Always use braces**, even for one-line bodies.
- `*` and `&` attach to the variable name: `QString *name`, `const QImage &image`.
- One space after keywords (`if (`, `for (`), none between a function name and `(`.
- File names are **lowercase** without separators, matching the main class: `timelinewidget.h`
  for `TimelineWidget` (KDE convention; also satisfies the no-all-caps rule).
- Qt keywords as macros: `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT` (built with `QT_NO_KEYWORDS`).
- Strings: `QStringLiteral` / `u"..."_s`-style literals, `QLatin1String` for comparisons;
  `QStringLiteral` only where a QString is needed.
- Member variables use the `m_` prefix; static members `s_`.
- `nullptr`, `override`, `Q_DECL_...`-free modern C++; `auto` only when the type is obvious.
- No `using namespace` in headers.
- Header include guards via `#pragma once` (accepted in KDE apps).
- Every file starts with an SPDX header (REUSE compliant):
  `SPDX-FileCopyrightText: 2026 <author>` + `SPDX-License-Identifier: GPL-3.0-or-later`.

**Migration note:** code written before this section used PascalCase file names and left
pointer alignment. The whole tree is converted mechanically (clang-format with the KDE config,
file renames, `Q_EMIT`/`Q_SIGNALS`/`Q_SLOTS`) in one commit after the overnight agent branches
are merged, to avoid merge conflicts. New code may already use the KDE style.

## 1. Formatting (enforced)
- `clang-format` with the repo's `.clang-format` (KDE/Qt-like, 4 spaces, 100 columns).
  Run `scripts/format.sh` before every commit. `.editorconfig` covers other files.
- UTF-8, LF line endings, newline at end of file, no trailing whitespace.
- `#pragma once` in every header.
- Include order (clang-format groups them): own header → project headers (`"core/..."`) → Qt → std.

## 1b. Hard rules from the project owner
- **No all-caps file names** (no `README.md`, `PLAN.md`, `LICENSE`...): docs are lowercase
  (`readme.md`, `plan.md`, `coding-style.md`). C++ sources are lowercase too (`timelinewidget.cpp`, KDE convention);
  `CMakeLists.txt` keeps the name CMake requires.
- **No emojis** anywhere: code, comments, docs, commit messages, UI strings.
- **Code produced by non-Claude tools/agents (Gemini, DeepSeek, ...) is untrusted**: it must be
  reviewed line by line, built with zero warnings and covered by tests before it is committed.

## 1c. Profile-ready code (see plan.md 5.9)
Shortcuts, menus, panel layout, settings pages and convention-dependent behaviour are DATA or go
through `ActionRegistry` / layout descriptions / an `InterfaceProfile` lookup. Never hard-code them
inside widgets. A future Plasma profile must be addable without touching panel internals.
A future Vim profile (plan.md 5.9b) needs every operation to be a command with explicit arguments
(targets, range, count) and navigation primitives as pure functions in `core` — keep editing logic
out of mouse handlers.

## 2. Naming
| Thing | Style | Example |
|---|---|---|
| Files | lowercase, one main class per file (KDE) | `timelinewidget.h/.cpp` |
| Classes, structs, enums, type aliases | PascalCase | `ClipData`, `enum class TrackType` |
| Enum values | PascalCase | `TrackType::Video` |
| Functions, methods, variables | camelCase | `renderFrame()`, `clipStart` |
| Private/protected data members | `m_` prefix | `m_playhead` |
| Plain public struct fields | no prefix | `clip.start` |
| Constants (`constexpr`) | `k` prefix | `kDefaultFps` |
| Namespaces | lower case | `kk`, `kk::media`, `kk::ui` |
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
- Each `src/<module>/` has a `readme.md`: responsibility, key classes, data flow,
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
- C++20, Qt 6.4 API. No compiler-specific extensions. Everything in `namespace kk`.
- Ownership: Qt parent/child for QObjects; `std::unique_ptr` / `std::shared_ptr` otherwise.
  No raw owning `new`/`delete` outside Qt parenting.
- `const` everything that can be. Pass big values by `const&`. `[[nodiscard]]` on functions whose
  result must be checked.
- Error handling: return `bool` + `QString* error`, or `std::optional`/expected-like struct.
  No exceptions crossing module boundaries. Log with `qCWarning(category)` (one logging category
  per module, e.g. `kk.media`).
- Threads: document it on the class (`/// Thread-safety: ...`). Render/decode threads only touch
  immutable snapshots (`std::shared_ptr<const TimelineData>`).
- No magic numbers: named `constexpr` constants with a comment.
- UI strings wrapped in `tr()` so the app can be translated later.

## 6. Tests & commits
- Logic gets QtTest unit tests in `tests/<module>/test_<thing>.cpp`.
- Commits: `<area>: <imperative summary>` (e.g. `timeline: add ripple trim`), body explains why.
