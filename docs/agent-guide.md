# Guide for coding agents (opencode and others)

Read all of this before touching the code. Written on 2026-11-07, when the work moved from
Claude to free models. If something breaks, read docs/recovery.md.

## The project

Kinoklip is a free Linux video editor (C++20, Qt 6, FFmpeg, OpenGL 3.3) that copies DaVinci
Resolve's layout, behaviour and keyboard shortcuts as closely as possible, with the KDE Breeze
look. Owner: SeniorMatt on GitHub (repo SeniorMatt/kinoklip, public). Released as one AppImage,
currently 1.1.6-beta.1. Pages: Media, Cut, Edit, Compositing, Color, Audio, Deliver; the
in-app names "Compositing" and "Audio" stay as they are.

Start here, in this order: readme.md, docs/directory-map.md, docs/coding-style.md, the
readme.md of the folder you work in (every src/ folder has one), docs/behavior-spec.md (the
rows about your area), docs/clean-room.md.

## Hard rules

1. **Clean room.** Never run, decompile or copy DaVinci Resolve. Use public documentation and
   general knowledge only, in your own words.
2. **No publishing.** Never push, tag, create releases, comment on GitHub issues or edit
   anything outside this repository. The owner decides releases.
3. **Never delete** files, branches, worktrees, tests or data you did not create in this task.
   Never `git reset --hard`, never `git push --force`.
4. **Never touch** `dist/`, `build-arch-release/`, `.cache/`, packaging files or
   `.github/` unless the task is about them.
5. **Privacy (docs/privacy.md).** The app never gathers or sends user information without the
   user's explicit consent.
6. **No secrets.** Never read or print key files, tokens or passwords.
7. No emojis in code, docs or commits. No ALL-CAPS file names. Never generate icon art.
8. One task at a time. If the task is unclear or bigger than expected, stop and report
   instead of guessing.

## How to work

1. Work in a git worktree, never directly on main:
   ```sh
   git worktree add -b <name> ../cutwise-wt/<name> main
   mkdir -p ../cutwise-wt/<name>/tests/data
   ln -s ~/cutwise/tests/data/generated ../cutwise-wt/<name>/tests/data/generated
   ```
2. Read the code you will change and its tests before editing. Find the existing way of doing
   something and reuse it; do not add a second mechanism.
   Building a new feature or fixing a hard problem? First find an open-source project that
   already does it (Kdenlive, Shotcut, Olive, OpenShot, Blender, Natron, Audacity, MLT, VLC,
   mpv, Neovim...), confirm that theirs really works well (docs, release notes, user reports,
   issues), and only then read their code and use it to make ours better. Licence rules for
   copying their code are in docs/clean-room.md.
3. Keep the change small and readable: the owner wants code that a human can read and extend.
   Follow docs/coding-style.md and run clang-format on the files you changed.
4. Build: `cmake -S . -B build && cmake --build build -j4` (ccache is used automatically).
   If a build looks stuck, run `pc-guard status` and wait; never kill it.
5. Commit on your branch with a short message saying what and why. Do not merge into main;
   the owner or a reviewer merges.

## Tests: stricter than usual

Free models make plausible mistakes. These rules exist to catch them.

1. **Every behaviour change needs a test** (Qt Test, in the matching tests/<area>/ folder, in
   the style of the tests next to it). A bug fix needs a test that fails without the fix:
   revert the fix, watch the test fail, restore the fix, watch it pass. Say in your report that
   you did this.
2. **Never weaken a test to make it pass.** Do not delete tests, loosen limits or thresholds,
   add QSKIP, QEXPECT_FAIL or sleeps, or change an expected value without explaining why the
   old one was wrong. If an existing test fails, your change is wrong until proven otherwise.
3. Run the tests related to your change, plus the neighbouring tests of the same area:
   `KK_NO_AUDIO=1 ctest --test-dir build -R '<pattern>' --output-on-failure`.
   Before saying you are done, run the whole suite once:
   `KK_NO_AUDIO=1 ctest --test-dir build -j3 -E '^test_cutperf$'`.
   Quote the exact failure text in your report; a rerun overwrites the log.
4. Timing tests can fail when the machine is busy (list in docs/recovery.md). Rerun such a
   test alone; if it passes alone, say so. Never call a failure flaky without rerunning it.
5. Report honestly: what you changed, what you tested, what failed, what you could not test
   (real GPU, real sound card, real desktop session). Never claim something works that you
   did not run.

## The behaviour spec

docs/behavior-spec.md is the record of how Kinoklip behaves. Rows are only ever appended
(never renumbered or removed). A new behaviour gets a new row with the next free number in the
fitting section, saying what it does and which test covers it. If you change behaviour that an
old row describes, add a new row that says it supersedes the old one.

## Planned for the next version

The list lives in [roadmap.md](roadmap.md) (section Now). Pick only what the owner or reviewer
assigns. Wider work on the pages ("Later") starts only when the owner asks. Not wanted:
background removal.
