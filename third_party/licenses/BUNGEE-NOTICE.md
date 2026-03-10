# Bungee Notice

Upstream:
- https://github.com/bungee-audio-stretch/bungee

Integrated role:
- optional stretch backend linked from repo-local static libraries when enabled

Observed upstream notice in `bungee/Bungee.h`:
- `Copyright (C) 2020-2026 Parabola Research Limited`
- `SPDX-License-Identifier: MPL-2.0`

This repository preserves the Mozilla Public License 2.0 text in
`BUNGEE-LICENSE.txt`.

Observed upstream dependencies relevant to this project:
- Eigen: compiled into the Bungee library and separately noted in `EIGEN-NOTICE.md`
- PFFFT: linked into the Bungee library and separately noted in `PFFFT-NOTICE.txt`
- cxxopts: used by the upstream sample command-line executable only, not by the `mlrVST` plugin build

If redistributed binaries include the Bungee backend, keep both this notice
and the MPL text in packaged releases.
