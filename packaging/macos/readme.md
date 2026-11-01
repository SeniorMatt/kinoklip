# macOS packaging (not implemented yet)

Plan for a macOS build of Kinoklip. Nothing here is automated yet.

## 1. Toolchain and dependencies
- Xcode command line tools (Apple Clang, C++20), CMake >= 3.24, Ninja.
- Qt 6.4.x for macOS (online installer; ships universal x86_64 + arm64 frameworks).
- FFmpeg 6.1 built from source **twice** (x86_64 and arm64, `--enable-shared --enable-gpl
  --enable-videotoolbox --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libaom
  --enable-libvpx --enable-libopus --enable-libmp3lame --enable-libwebp`) and its codec libs
  likewise, then merged with `lipo -create`. Homebrew bottles are single-architecture and
  depend on `/opt/homebrew` paths, so they are only good for local development.
  Alternative: vcpkg with triplets `x64-osx-dynamic` and `arm64-osx-dynamic`, merged with lipo.

## 2. Universal build
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0`
- `MACOSX_BUNDLE ON` is already set on the target; add an `Info.plist` template
  (bundle id `org.kinoklip.Kinoklip`, version, `CFBundleDocumentTypes` for `.kkproj` and the
  video types, `NSMicrophoneUsageDescription` if recording is added) and an `.icns` rendered
  from `packaging/linux/icons/org.kinoklip.Kinoklip.svg` (`iconutil -c icns`).
- Hardware: VideoToolbox decode/encode via FFmpeg. OpenGL 3.3 core works but is deprecated on
  macOS; Metal (via QRhi or a Metal backend of the compositor) is the long-term path.

## 3. Deployment
1. `macdeployqt build/Kinoklip.app` (copies Qt frameworks + cocoa platform plugin, styles,
   imageformats, iconengines, multimedia/ffmpeg) and then copy the FFmpeg dylibs into
   `Contents/Frameworks`.
2. Fix install names with `install_name_tool -id @rpath/... ` / `-change` and add
   `-add_rpath @executable_path/../Frameworks`; check with `otool -L` that nothing points to
   `/opt/homebrew` or `/usr/local`.
3. Verify both slices: `lipo -info` on every binary in the bundle.

## 4. Signing and notarization
1. `codesign --deep --force --options runtime --timestamp --entitlements kinoklip.entitlements
   --sign "Developer ID Application: ..." Kinoklip.app` (sign nested dylibs/frameworks first;
   `--deep` alone is discouraged). Entitlements: none special unless JIT/unsigned memory is
   needed (it is not).
2. Package as `.dmg` (`hdiutil create` or `create-dmg`), sign the dmg.
3. `xcrun notarytool submit Kinoklip.dmg --keychain-profile <profile> --wait`, then
   `xcrun stapler staple Kinoklip.dmg`.
4. Test on a clean machine with Gatekeeper enabled (`spctl -a -vv Kinoklip.app`).

## 5. CI
- GitHub Actions `macos-14` (arm64) runner; cross-build x86_64 in the same job; cache the
  FFmpeg build; notarization secrets in the repository's encrypted secrets.
