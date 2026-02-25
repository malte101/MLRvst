# Documentation

This folder contains build and technical docs for the current `mlrVST` codebase.

For the GitHub project overview, start with the root README:

- `../README.md`

## Main Guides

- `USER_MANUAL.md`
  - End-user operation guide (GUI + monome workflows)
- `BUILD.md`
  - Platform build steps
  - Build options
  - Release packaging
- `CLION_SETUP.md`
  - CLion project setup and troubleshooting
- `AUDIO_ENGINE_DOCS.md`
  - Audio engine architecture and behavior
- `SERIALOSC_REFERENCE.md`
  - SerialOSC message reference
- `MODERNIZATION_GUIDE.md`
  - Historical modernization notes
- `SUMMARY.md`
  - High-level modernization summary

## Current Project Facts

- Plugin formats: VST3 + AU (macOS)
- Audio strips: 6
- Build system: CMake with Makefile helpers
- Release packaging script: `../scripts/package_release_macos.sh`

## Licensing

- Project code: `../LICENSE`
- Third-party terms and redistribution notes: `../THIRD_PARTY_NOTICES.md`

If you distribute binaries, include the relevant notice files.
