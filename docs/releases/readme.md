# Release notes

One file per release, `v<version>.md` (e.g. `v0.4.1-alpha.1.md`); its text is the GitHub
release description. Every release is a GitHub **pre-release** until the owner decides on 1.0
(docs/roadmap.md).

## Before publishing
Run the memory checks first ([contributing.md](../contributing.md#memory-leaks)):
`scripts/check_leaks.sh` (clean exit, no new suppression of our own code) and `ctest -L soak`
(passes, no QEXPECT_FAIL left). Then the whole test suite, then package.

## Assets to upload
- `Kinoklip-x86_64.AppImage`
- `Kinoklip-x86_64.AppImage.zsync` (Gear Lever and [AppImageUpdate](https://github.com/AppImageCommunity/AppImageUpdate) find updates through it;
  without it the update check fails)
- `SHA256SUMS.txt` (`sha256sum Kinoklip-x86_64.AppImage > SHA256SUMS.txt`)

Both made by `scripts/package_linux.sh` in the container, see
[packaging/linux/readme.md](../../packaging/linux/readme.md). Keep the file names without the
version: the update information inside the AppImage looks for `Kinoklip-*x86_64.AppImage.zsync`.

## Download link in the main readme (update every release)
The "Download here" heading of the repository's [readme.md](../../readme.md) links straight to
the AppImage of the newest release, with the version in the URL:
`https://github.com/SeniorMatt/kinoklip/releases/download/v<version>/Kinoklip-x86_64.AppImage`.
Change the version there with each release. GitHub's `/releases/latest/download/...` URL would
not need that, but it skips pre-releases, and every release is a pre-release for now.

## Download block (start of every release note, from 0.4.1 on)

```markdown
## Download and run (Linux x86_64)
1. Download `Kinoklip-x86_64.AppImage`.
2. Open it with [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) (adds Kinoklip to the application menu and keeps it updated), or make it executable: right-click > Properties > Permissions > "Is executable", or `chmod +x Kinoklip-x86_64.AppImage`.
3. Run it: from the menu, by double-clicking the file, or `./Kinoklip-x86_64.AppImage`.

Needs: x86_64 Linux, OpenGL 3.3, FUSE (on almost every desktop system). **If double-clicking shows nothing, FUSE is missing** (the AppImage runtime then only prints to a terminal): install `fuse3` (Debian/Ubuntu: `sudo apt install fuse3`), or run `./Kinoklip-x86_64.AppImage --appimage-extract-and-run` in a terminal. Wayland and X11. `SHA256SUMS.txt` has checksums.

Coming from 0.4.0 or older (the `.run` file): the old menu entry still points at it. Remove it with `./Kinoklip-x86_64.run --uninstall-desktop-entry` (or delete `~/.local/share/applications/org.kinoklip.Kinoklip.desktop`); `./Kinoklip-x86_64.run --uninstall-cache` frees the unpacked copies in `~/.cache/kinoklip`.
```
