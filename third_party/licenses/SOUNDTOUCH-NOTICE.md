# SoundTouch Notice

Upstream:
- https://www.surina.net/soundtouch

Integrated role:
- optional dynamic time-stretch backend when a packaged binary links
  `libSoundTouch`

License:
- LGPL v2.1

The macOS release packager copies the upstream `COPYING.TXT` file into release
archives when a built plugin bundle links SoundTouch. If you redistribute a
build that links SoundTouch, keep both this notice and the copied
`SoundTouch-COPYING.TXT` file with the package.
