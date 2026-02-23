# Third-Party Notices

This repository contains project code under MIT (`/LICENSE`) and references/includes third-party components with their own licenses.

This document is an engineering notice, not legal advice.

## Project License

- `mlrVST` project code in this repository: MIT (`/LICENSE`)

## Third-Party Components

### JUCE Framework

- Upstream: https://github.com/juce-framework/JUCE
- Role: framework used to build plugin binaries.
- License model: JUCE is distributed under GPLv3 or a commercial JUCE license (depending on how you license your use).
- Notes:
  - If you distribute binaries built with JUCE, you must comply with the JUCE terms for your chosen license path.
  - JUCE is not committed in this repository by default, but is required for builds.

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
