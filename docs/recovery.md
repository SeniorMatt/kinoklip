# If something goes wrong

Written on 2026-11-07, when work moved from Claude to opencode (free models). Read this first
when a change breaks the build, the tests or the app.

## Known good points

| What | Where |
|---|---|
| Last published release | tag `v1.1.6-beta.1` (Fedora start fix) |
| Main before the opencode hand-off | this commit's parent; `git log --grep "recovery: how to undo"` finds it |
| Release before that | tag `v1.1.5-beta.1` (Bog fixes) |

Local main was 3 commits ahead of GitHub at hand-off: the `distrofit` merge, which bundles
nearly every system library and adds `scripts/check_distros.sh`. It is not released and not
pushed.

## Undo a bad change

1. See what changed: `git log --oneline -20` and `git diff <good-commit>..HEAD --stat`.
2. Undo one commit, keeping the history: `git revert <commit>` (a merge: `git revert -m 1 <merge>`).
3. Throw away uncommitted edits in one file: `git restore <file>`. Check `git diff <file>`
   first; there is no way back.
4. Never `git reset --hard`, `git push --force` or delete tags on GitHub. Ask the owner.
5. A worktree went wrong: leave main alone and remove the worktree only after checking that
   nothing in it is needed (`git -C <worktree> status`, and whether its branch is merged).

## Check that main is healthy again

```sh
cmake -S . -B build && cmake --build build -j4
KK_NO_AUDIO=1 ctest --test-dir build -j3 -E '^test_cutperf$'
```

All tests must pass on a quiet machine. Timing tests (test_largeproject, test_firstplay,
test_playbackgap, test_realworld, test_deliverpage, test_videodecoder, test_colorpage,
test_proxies, test_export) can fail under heavy load: rerun them alone before blaming a change.

The release build and the Breeze style only exist in the Arch container (`kk-arch`); some
layout and colour bugs show only there. See packaging/linux/readme.md.

## A published release is broken

- Do not delete it. Fix forward: a new patch version (1.1.7 ...) with the fix, published only
  with the owner's go (`scripts/publish_release.sh <tag> "<title>"`).
- Users can always download an older AppImage from the releases page.
- Tell the reporter on the GitHub issue, short and plain, in the owner's voice.

## Builds look stuck

The laptop's pc-guard pauses builds (SIGSTOP) when the CPU is hot, the disk is full or the
owner's son is playing. Run `pc-guard status`; never kill or restart a paused build.
