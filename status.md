# Cutwise — live status log
(newest at the bottom; times are local)

- 01:05 Toolchain installed (Qt 6.4.2, FFmpeg 6.1, CMake 3.28, x264/AAC available).
- 01:15 plan.md written; skeleton + git repo created.
- 01:25 Coding standard added (docs/coding-style.md, .clang-format, scripts/format.sh).
- 01:35 Phase 1 started: agents 'foundation' (core model + app shell) and 'media' (FFmpeg engine) running in parallel.
- 01:36 Downloading AppImage tools (linuxdeploy/appimagetool) was blocked by the safety filter (external executables). Asked user; fallback = own self-extracting single-file launcher.
- 01:40 User: agents at medium effort from now on, keep usage lean. Scheduled auto-resume at 03:22 + hourly heartbeat.
- 01:36 Command logging hook added: every Bash command / file write by me and all agents → ~/cutwise/logs/commands.log (watch live: tail -f ~/cutwise/logs/commands.log).
- 01:45 User: performance first (readability via comments), GPU acceleration on all vendors. PLAN + CODING_STYLE updated; running agents notified (media: hw decode NVDEC/VAAPI/Vulkan + raw YUV frames; foundation: GL 3.3 context, cheap hot paths). gh CLI installed for GitHub repo.
- 01:45 Target machine noted: T14s Gen 2 (i5-1145G7, Iris Xe, KDE Linux/Wayland) → VAAPI primary; media agent informed.
- 02:06 Heartbeat: foundation 3 commits (core model, app shell, tests+READMEs) — finishing; media 1 commit (probe + frame-exact video + sample-exact audio decoders), working on hw decode/cache. Repo name still being chosen with user.
- 02:10 Name approved: **Kinoklip**. Private GitHub repo created: https://github.com/SeniorMatt/kinoklip (main pushed). In-code rename Cutwise→Kinoklip happens right after the foundation merge (avoids conflicts with running agents).
- 02:19 Foundation merged (10/10 tests, 0 warnings, layout screenshot verified). Renamed Cutwise -> Kinoklip (namespace kk, macros KK_, .kkproj, binary 'kinoklip'); doc files now lowercase. No-emoji / no-all-caps / review-external-code rules added.
- 02:21 Phase 2 started (effort medium): agents 'timeline' (editing widget, tools, EditOps) and 'panels' (inspector, media pool, effects library, dialogs). 'media' still running. Shared drag-drop MIME contract added. Scheduled check-ins re-created for the new file names.
- 02:28 Media merged (15/15 tests, 0 warnings): exact-seek decoders, hw decode VAAPI/CUDA/Vulkan with SW fallback, raw YUV frames, MediaCache thumbnails/waveforms. Note: on this dev box GPU device access alternates with the other logged-in user (tisha) - not an issue on a single-user laptop.
