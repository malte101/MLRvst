# Third-Party Notices

This repository contains project code under AGPLv3 (`/LICENSE`) and references/includes third-party components with their own licenses.

This document is an engineering notice, not legal advice.

## Project License

- `mlrVST` project code in this repository: GNU Affero General Public License v3.0 (`/LICENSE`)

## Upstream Provenance

### Original mlrVST (Ewan Hemingway / hemmer)

- Upstream: https://github.com/hemmer/mlrVST
- Role: original project this repository modernizes and continues.
- Upstream license status observed locally on 2026-03-10:
  - upstream commit `1b81a371c777620ebdab48374c99ffc2b57cefca` added a top-level MIT license on 2026-02-24.
- Preserved local notice files:
  - `/NOTICE`
  - `/UPSTREAM_PROVENANCE.md`
  - `/third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt`
- Redistribution note:
  - Do not remove the original mlrVST attribution to Ewan Hemingway (`hemmer`) or the original mlr attribution to Brian Crabtree / monome from source or packaged binaries.

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
  - Preserve `/third_party/licenses/ESSENTIA-NOTICE.md` together with `/LICENSE` when native Essentia code is included in redistributed binaries.

### Bungee

- Upstream: https://github.com/bungee-audio-stretch/bungee
- Role: optional tempo/pitch stretch backend for Loop/Gate swing and Flip tempo matching.
- License model: MPL-2.0.
- Notes:
  - The build now prefers a persistent repo-local native prefix at `/third_party/_native/bungee-prefix`.
  - The helper script `/scripts/bootstrap_native_deps.sh` rebuilds Bungee static libraries and headers into that prefix.
  - The project also still supports SoundTouch as an alternative backend.
  - Preserve `/third_party/licenses/BUNGEE-NOTICE.md` and `/third_party/licenses/BUNGEE-LICENSE.txt` with redistributions that include the Bungee backend.
  - Mozilla's MPL 2.0 text defines AGPLv3 as a `Secondary License`, and Mozilla's FAQ describes MPL code being combined with GPL-family code in a Larger Work. Review the upstream notice terms before redistribution; this file is not legal advice.

### PFFFT (via Bungee build)

- Upstream: https://bitbucket.org/jpommier/pffft.git
- Role: Fourier helper static library linked as `libpffft.a` by the Bungee build.
- License/notice model: upstream header notice with FFTPACK/NCAR redistribution terms.
- Preserved local notice file:
  - `/third_party/licenses/PFFFT-NOTICE.txt`

### SoundTouch

- Upstream: https://www.surina.net/soundtouch
- Role: optional dynamic stretch backend when linked.
- License model: LGPL-2.1.
- Notes:
  - Current macOS builds on this machine link Homebrew's `libSoundTouch` when available.
  - Preserve `/third_party/licenses/SOUNDTOUCH-NOTICE.md` and the copied `SoundTouch-COPYING.TXT` notice in redistributed archives that link SoundTouch.

### LibPyin

- Upstream: https://github.com/xstreck1/LibPyin
- Path: `/third_party/LibPyin`
- Role: offline monophonic pitch detection for Loop-mode `PM` / `PS` analysis.
- License model: GPL-3.0-or-later for LibPyin wrapper/source tree, with bundled upstream notices for pYIN and Vamp components.
- Notes:
  - This repository vendors the LibPyin source tree directly under `/third_party/LibPyin`.
  - Release archives should include:
    - `/third_party/LibPyin/LICENSE`
    - `/third_party/LibPyin/source/LICENSE_PYIN`
    - `/third_party/LibPyin/source/LICENSE_VAMP`
  - LibPyin is only used in the existing async offline analysis job for loop strips. It is not run on the audio thread and does not add host-reported plug-in latency.

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
2. `/NOTICE`
3. `/UPSTREAM_PROVENANCE.md`
4. `/THIRD_PARTY_NOTICES.md`
5. Relevant upstream license texts and notice files for bundled or linked code:
   - `/third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt`
   - `/third_party/MoogLadders-main/LICENSE`
   - `/third_party/LibPyin/LICENSE`
   - `/third_party/LibPyin/source/LICENSE_PYIN`
   - `/third_party/LibPyin/source/LICENSE_VAMP`
   - `/JUCE/LICENSE.md`
   - `/third_party/licenses/BUNGEE-LICENSE.txt`
   - `/third_party/licenses/BUNGEE-NOTICE.md`
   - `/third_party/licenses/ESSENTIA-NOTICE.md`
   - `/third_party/licenses/PFFFT-NOTICE.txt`
   - `/third_party/licenses/SOUNDTOUCH-NOTICE.md`
   - `SoundTouch-COPYING.TXT` when a packaged binary links `libSoundTouch`

The script `/scripts/package_release_macos.sh` follows this policy and preserves the original `hemmer/mlrVST` attribution in packaged releases.
