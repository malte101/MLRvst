#!/usr/bin/env bash
# Package a MinGW cross-built Windows VST3 into release/.
#
# The repo's package_release_windows.ps1 is the on-Windows path; this is its
# equivalent for the macOS cross-build (no PowerShell required). It produces the
# same archive layout: a single top-level folder containing the .vst3 bundle
# (with the MinGW runtime DLLs beside the binary in Contents/x86_64-win),
# the notice files, a LICENSES/ directory and RELEASE_MANIFEST.txt.
#
# Usage: scripts/package_release_windows_crossbuild.sh [BUILD_DIR] [OUT_DIR]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/cmake-build-windows}"
OUT_DIR="${2:-${REPO_ROOT}/release}"

BUNDLE=""
for candidate in \
    "${BUILD_DIR}/mlrVST_artefacts/Release/VST3/mlrVST.vst3" \
    "${BUILD_DIR}/mlrVST_artefacts/VST3/mlrVST.vst3"; do
    if [[ -d "${candidate}" ]]; then
        BUNDLE="${candidate}"
        break
    fi
done

if [[ -z "${BUNDLE}" ]]; then
    echo "Could not find a Windows VST3 bundle under ${BUILD_DIR}" >&2
    exit 1
fi

COMMIT="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
SHORT_SHA="${COMMIT:0:7}"
TIMESTAMP="$(date -u +%Y%m%d-%H%M%S)"
PACKAGE_NAME="mlrVST-windows-x64-vst3-${TIMESTAMP}-${SHORT_SHA}"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mlrvst-winrelease-XXXXXX")"
cleanup() { rm -rf "${STAGE_DIR}"; }
trap cleanup EXIT

PACKAGE_DIR="${STAGE_DIR}/${PACKAGE_NAME}"
mkdir -p "${PACKAGE_DIR}"

cp -R "${BUNDLE}" "${PACKAGE_DIR}/mlrVST.vst3"

# --- MinGW runtime DLLs must sit next to the plugin binary ---
RUNTIME_DIR="${PACKAGE_DIR}/mlrVST.vst3/Contents/x86_64-win"
mkdir -p "${RUNTIME_DIR}"

MINGW_BIN="$(dirname "$(command -v x86_64-w64-mingw32-g++)")"
MINGW_ROOT="$(cd "${MINGW_BIN}/.." && pwd)"
BUNDLED_RUNTIMES=()

for runtime in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    if [[ -f "${RUNTIME_DIR}/${runtime}" ]]; then
        BUNDLED_RUNTIMES+=("${runtime}")
        continue
    fi

    # Only ever take the x86_64 toolchain copies — the formula also ships i686.
    found="$(find "${MINGW_ROOT}" /opt/homebrew/Cellar/mingw-w64 -path '*toolchain-x86_64*' \
                  -name "${runtime}" -type f 2>/dev/null | head -1 || true)"
    if [[ -n "${found}" ]]; then
        cp "${found}" "${RUNTIME_DIR}/${runtime}"
        BUNDLED_RUNTIMES+=("${runtime}")
    else
        echo "warning: ${runtime} not found in the MinGW toolchain" >&2
    fi
done

# --- Notices, matching the macOS packaging script ---
for notice in LICENSE NOTICE UPSTREAM_PROVENANCE.md THIRD_PARTY_NOTICES.md README.md; do
    [[ -f "${REPO_ROOT}/${notice}" ]] && cp "${REPO_ROOT}/${notice}" "${PACKAGE_DIR}/"
done

mkdir -p "${PACKAGE_DIR}/LICENSES"
copy_license() {
    [[ -f "${REPO_ROOT}/$1" ]] && cp "${REPO_ROOT}/$1" "${PACKAGE_DIR}/LICENSES/$2"
}
copy_license "third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt" "hemmer-mlrVST-MIT-LICENSE.txt"
copy_license "third_party/MoogLadders-main/LICENSE"               "MoogLadders-main-LICENSE.txt"
copy_license "third_party/LibPyin/LICENSE"                        "LibPyin-LICENSE.txt"
copy_license "third_party/LibPyin/source/LICENSE_PYIN"            "LibPyin-PYIN-LICENSE.txt"
copy_license "third_party/LibPyin/source/LICENSE_VAMP"            "LibPyin-VAMP-LICENSE.txt"
copy_license "third_party/licenses/BUNGEE-LICENSE.txt"            "BUNGEE-LICENSE.txt"
copy_license "third_party/licenses/BUNGEE-NOTICE.md"              "BUNGEE-NOTICE.md"
copy_license "third_party/licenses/ESSENTIA-NOTICE.md"            "ESSENTIA-NOTICE.md"
copy_license "third_party/licenses/EIGEN-NOTICE.md"               "EIGEN-NOTICE.md"
copy_license "third_party/licenses/PFFFT-NOTICE.txt"              "PFFFT-NOTICE.txt"
copy_license "third_party/licenses/SOUNDTOUCH-NOTICE.md"          "SOUNDTOUCH-NOTICE.md"
copy_license "third_party/signalsmith-stretch/LICENSE.txt"        "signalsmith-stretch-LICENSE.txt"
copy_license "third_party/signalsmith-linear/LICENSE.txt"         "signalsmith-linear-LICENSE.txt"
copy_license "JUCE/LICENSE.md"                                    "JUCE-LICENSE.md"

runtime_field="none"
if (( ${#BUNDLED_RUNTIMES[@]} > 0 )); then
    runtime_field="$(IFS=', '; echo "${BUNDLED_RUNTIMES[*]}")"
fi

cat > "${PACKAGE_DIR}/RELEASE_MANIFEST.txt" <<EOF
Product: mlrVST
Platform: Windows x64
Format: VST3
Commit: ${COMMIT}
Workflow run: n/a
Bundled runtimes: ${runtime_field}
Built at (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)
Built with: MinGW-w64 cross-build from macOS
EOF

mkdir -p "${OUT_DIR}"
ZIP_PATH="${OUT_DIR}/${PACKAGE_NAME}.zip"
rm -f "${ZIP_PATH}"
(cd "${STAGE_DIR}" && zip -qr "${ZIP_PATH}" "${PACKAGE_NAME}")

echo "${ZIP_PATH}"
