This directory is the persistent local home for native mlrVST build dependencies.

Current intent:
- `essentia-prefix/`: lightweight native Essentia install used by Flip/MLR offline analysis
- `bungee-prefix/`: native Bungee headers and static libraries used by the stretch backend

These assets are local build inputs, not normal project source.
They are ignored by Git by default so the repo does not accidentally commit generated third-party binaries.

To rebuild/populate them:

```bash
./scripts/bootstrap_native_deps.sh
```

`CMakeLists.txt` prefers these repo-local prefixes over temporary `/tmp/...` paths so builds keep working after temp cleanup.
