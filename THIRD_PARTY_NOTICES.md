# Third-Party Notices

This repository contains project code under AGPLv3 (`/LICENSE`) and references/includes third-party components with their own licenses.

This document is an engineering notice, not legal advice.

## Project License

- `mlrVST` project code in this repository: GNU Affero General Public License v3.0 (`/LICENSE`)

## Third-Party Components

### JUCE Framework

- Upstream: https://github.com/juce-framework/JUCE
- Role: framework used to build plugin binaries.
- License model: JUCE 8 is dual-licensed under AGPLv3 or a commercial JUCE license.
- Notes:
  - This repository now takes the AGPLv3 path for JUCE-based distribution.
  - JUCE is not committed in this repository by default, but is required for builds.

### Essentia

- Upstream: https://github.com/MTG/essentia
- Role: offline Flip/loop pitch and tempo analysis.
- License model: AGPLv3.
- Notes:
  - Current integration links against a native Essentia build when available.
  - The build now prefers a persistent repo-local native prefix at `/third_party/_native/essentia-prefix`.
  - The helper script `/scripts/bootstrap_native_deps.sh` rebuilds a lightweight static Essentia into that prefix.
  - Generated third-party binaries in `/third_party/_native` are local build assets and are ignored by Git by default.

### Bungee

- Upstream: https://github.com/bungee-audio-stretch/bungee
- Role: optional tempo/pitch stretch backend for Loop/Gate swing and Flip tempo matching.
- License model: MPL-2.0.
- Notes:
  - The build now prefers a persistent repo-local native prefix at `/third_party/_native/bungee-prefix`.
  - The helper script `/scripts/bootstrap_native_deps.sh` rebuilds Bungee static libraries and headers into that prefix.
  - The project also still supports SoundTouch as an alternative backend.
  - Mozilla's MPL 2.0 text defines AGPLv3 as a `Secondary License`, and Mozilla's FAQ describes MPL code being combined with GPL-family code in a Larger Work. Review the upstream notice terms before redistribution; this file is not legal advice.

### MoogLadders (vendored source tree)

- Path: `/third_party/MoogLadders-main`
- Upstream: https://github.com/ddiakopoulos/MoogLadders
- Default project license for that upstream tree: Unlicense (`/third_party/MoogLadders-main/LICENSE`)
- Important: this tree contains multiple model files with per-file licensing notes/headers.

#### Models used by runtime code

- `StilsonModel.h`
  - Header indicates public-domain/Unlicense style grant.
- `HuovilainenModel.h`
  - Header states it is based on CSound5 implementation and references LGPL terms.

If you ship binaries that include `HuovilainenModel.h`-derived code, review and satisfy applicable LGPL obligations.

Current default CMake configuration sets:

- `MLRVST_ENABLE_HUOVILAINEN=OFF`

With that default, the "MOOG H" selection falls back to the Stilson model at runtime.

#### Other files present in the vendored tree

Some files in `/third_party/MoogLadders-main/src` and `/third_party/MoogLadders-main/third_party` have their own headers/licenses (ISC, BSD-like, custom notices, etc.). Even if not linked into runtime, keep their license headers intact when redistributing source.

## Binary Release Packaging Policy

Release archives should include:

1. `/LICENSE`
2. `/THIRD_PARTY_NOTICES.md`
3. Relevant upstream license texts for bundled vendor code (at minimum `/third_party/MoogLadders-main/LICENSE`)

The script `/scripts/package_release_macos.sh` follows this policy.
