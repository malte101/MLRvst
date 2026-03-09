This file is kept for path continuity only.

The current native dependency model is documented in:

- `/third_party/_native/README.md`
- `/THIRD_PARTY_NOTICES.md`

Current state:
- mlrVST uses a native C++ Essentia build, not the old Python package path.
- The preferred persistent install prefix is `/third_party/_native/essentia-prefix`.
- `/scripts/bootstrap_native_deps.sh` rebuilds the local Essentia and Bungee prefixes when needed.
