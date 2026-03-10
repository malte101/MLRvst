# Eigen Notice

Upstream:
- https://eigen.tuxfamily.org/

Integrated role:
- header-only linear algebra and tensor code compiled into the Bungee and native Essentia code paths used by this project

Observed upstream license model:
- MPL-2.0

Notes:
- Eigen is header-only, so its code is compiled directly into binaries that use the Bungee and/or native Essentia backends.
- This repository already preserves the full Mozilla Public License 2.0 text in `BUNGEE-LICENSE.txt`.
- If redistributed binaries include native Bungee and/or native Essentia code, keep both this notice and the bundled MPL 2.0 text in the packaged release.
