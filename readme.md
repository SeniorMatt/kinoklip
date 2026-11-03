<p align="center"><img src="docs/images/logo/kinoklip-icon.png" width="128" height="128" alt="Kinoklip icon"></p>
<p align="center"><img src="docs/images/logo/kinoklip-text-horizontal.png" width="480" height="104" alt="Kino Klip"></p>

<h1 align="center"><a href="https://github.com/SeniorMatt/kinoklip/releases/download/v1.0.0-beta.1/Kinoklip-x86_64.AppImage">Download here</a></h1>
<p align="center"><a href="docs/guide/readme.md">How to use Kinoklip</a></p>

A non-linear video editor written from scratch in C++20 with Qt 6 Widgets and FFmpeg. The layout,
behaviour and default shortcuts follow DaVinci Resolve's Edit page, with the KDE Breeze look.
Kinoklip is an independent project and is not affiliated with Blackmagic Design.

All releases are alpha or beta pre-releases for now: expect bugs and keep backups.

![The Kinoklip Edit page: media pool, source and timeline viewers, inspector and a multi-track timeline](docs/images/edit-page.png)

## Documentation

- [Building](docs/building.md): build from source (Arch container or host Qt), useful command-line options
- [Contributing](docs/contributing.md): guidelines for code, shortcuts, naming and tests
- [Coding style](docs/coding-style.md): naming, comments, file layout
- [Directory map](docs/directory-map.md): what lives where in the source tree
- [Roadmap](docs/roadmap.md): the milestones ahead
- [Plan and architecture](plan.md): module contracts and design rules
- [Behaviour spec](docs/behavior-spec.md): how each feature behaves, with test references
- [Resolve keymap](docs/keymap-resolve-windows.md): default keyboard shortcuts
- [Release notes](docs/releases/): changes in each release
- [Linux packaging](packaging/linux/readme.md): building the AppImage
- [Clean-room policy](docs/clean-room.md): what may and may not be used to build Kinoklip
- [Attribution](attribution.md): every asset with its author and licence

## Support
If you want to support the project: [ko-fi.com/seniormatthew](https://ko-fi.com/seniormatthew), it will be much appreciated.

## License
The code is GPL-3.0-or-later, see [license.md](license.md). The Kinoklip name, icon and logos are
not under the GPL: forks must replace them, see [logo-license.md](logo-license.md). Release bundles
include GPL FFmpeg components; third-party notices are inside the bundle under
`usr/share/doc/kinoklip/`.

## Legal
- **Independent project.** Kinoklip is not affiliated with, sponsored by or endorsed by Blackmagic
  Design. DaVinci Resolve, Fusion, Fairlight and Blackmagic Design are trademarks or registered
  trademarks of Blackmagic Design Pty Ltd. They are named here only to describe the workflow and
  shortcuts Kinoklip follows. Other names (Qt, KDE, FFmpeg, YouTube, Vimeo and others) are
  trademarks of their owners.
- **Clean room.** Kinoklip is written from general knowledge of how Resolve behaves and from public,
  open specifications. It contains no Blackmagic code, icons, images, fonts, presets, LUTs or
  documentation text, and Resolve is never installed, run or decompiled to build it. The rules and
  their limits are in [docs/clean-room.md](docs/clean-room.md); every asset and its licence is in
  [attribution.md](attribution.md).
- **No warranty.** Kinoklip is provided "as is", without any warranty, as stated in the GPL
  (sections 15 and 16 of [license.md](license.md)). It is beta software: keep backups.
- **Codecs.** Some video and audio formats (for example H.264, H.265 and AAC) are covered by
  patents in some countries. The bundles use open-source implementations of them; whether you need
  a patent licence to use them depends on where you live and what you do with them, and is your
  responsibility.

## AI disclosure
This project was built with AI: the code, tests and documentation were written by AI, directed
and tested by the project owner. The only exception is the icon, which was drawn by hand.
**We aren't appreciating AI art here.**
