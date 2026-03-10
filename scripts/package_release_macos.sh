#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./scripts/package_release_macos.sh [--build-dir DIR] [--config CONFIG]

Options:
  --build-dir DIR   Build directory containing mlrVST_artefacts (default: auto-detect)
  --config CONFIG   Build config folder (default: Release)
  -h, --help        Show this help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR=""
CONFIG="Release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --config)
            CONFIG="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This packager currently supports macOS only." >&2
    exit 1
fi

if [[ -z "${BUILD_DIR}" ]]; then
    have_cmake_build_release=0
    have_build_dir=0

    if [[ -d "${REPO_ROOT}/cmake-build-release/mlrVST_artefacts/${CONFIG}" ]]; then
        have_cmake_build_release=1
    fi

    if [[ -d "${REPO_ROOT}/Build/mlrVST_artefacts/${CONFIG}" ]]; then
        have_build_dir=1
    fi

    if [[ "${have_cmake_build_release}" -eq 1 && "${have_build_dir}" -eq 1 ]]; then
        echo "Multiple build dirs detected (cmake-build-release and Build)." >&2
        echo "Pass --build-dir explicitly so packaging matches the binary you reviewed." >&2
        exit 1
    elif [[ "${have_cmake_build_release}" -eq 1 ]]; then
        BUILD_DIR="cmake-build-release"
    elif [[ "${have_build_dir}" -eq 1 ]]; then
        BUILD_DIR="Build"
    else
        echo "Could not auto-detect build dir. Pass --build-dir explicitly." >&2
        exit 1
    fi
fi

ARTEFACTS_DIR="${REPO_ROOT}/${BUILD_DIR}/mlrVST_artefacts/${CONFIG}"
VST3_BUNDLE="${ARTEFACTS_DIR}/VST3/mlrVST.vst3"
AU_BUNDLE="${ARTEFACTS_DIR}/AU/mlrVST.component"

if [[ ! -d "${VST3_BUNDLE}" && ! -d "${AU_BUNDLE}" ]]; then
    echo "No plugin bundles found under: ${ARTEFACTS_DIR}" >&2
    exit 1
fi

for required in \
    "${REPO_ROOT}/LICENSE" \
    "${REPO_ROOT}/NOTICE" \
    "${REPO_ROOT}/UPSTREAM_PROVENANCE.md" \
    "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" \
    "${REPO_ROOT}/third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt" \
    "${REPO_ROOT}/third_party/licenses/BUNGEE-LICENSE.txt" \
    "${REPO_ROOT}/third_party/licenses/BUNGEE-NOTICE.md" \
    "${REPO_ROOT}/third_party/licenses/ESSENTIA-NOTICE.md" \
    "${REPO_ROOT}/third_party/licenses/EIGEN-NOTICE.md" \
    "${REPO_ROOT}/third_party/licenses/PFFFT-NOTICE.txt" \
    "${REPO_ROOT}/third_party/licenses/SOUNDTOUCH-NOTICE.md" \
    "${REPO_ROOT}/third_party/MoogLadders-main/LICENSE" \
    "${REPO_ROOT}/third_party/LibPyin/LICENSE" \
    "${REPO_ROOT}/third_party/LibPyin/source/LICENSE_PYIN" \
    "${REPO_ROOT}/third_party/LibPyin/source/LICENSE_VAMP" \
    "${REPO_ROOT}/JUCE/LICENSE.md"; do
    if [[ ! -f "${required}" ]]; then
        echo "Missing required notice file: ${required}" >&2
        exit 1
    fi
done

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
ARCH="$(uname -m)"
OUT_DIR="${REPO_ROOT}/release"
mkdir -p "${OUT_DIR}"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mlrvst-release-XXXXXX")"
cleanup() {
    rm -rf "${STAGE_DIR}"
}
trap cleanup EXIT

copy_common_notices() {
    local target_dir="$1"
    mkdir -p "${target_dir}/LICENSES"
    cp "${REPO_ROOT}/LICENSE" "${target_dir}/"
    cp "${REPO_ROOT}/NOTICE" "${target_dir}/"
    cp "${REPO_ROOT}/UPSTREAM_PROVENANCE.md" "${target_dir}/"
    cp "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" "${target_dir}/"

    cp "${REPO_ROOT}/third_party/licenses/hemmer-mlrVST-MIT-LICENSE.txt" \
       "${target_dir}/LICENSES/hemmer-mlrVST-MIT-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/MoogLadders-main/LICENSE" \
       "${target_dir}/LICENSES/MoogLadders-main-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/LibPyin/LICENSE" \
       "${target_dir}/LICENSES/LibPyin-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/LibPyin/source/LICENSE_PYIN" \
       "${target_dir}/LICENSES/LibPyin-PYIN-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/LibPyin/source/LICENSE_VAMP" \
       "${target_dir}/LICENSES/LibPyin-VAMP-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/licenses/BUNGEE-LICENSE.txt" \
       "${target_dir}/LICENSES/BUNGEE-LICENSE.txt"
    cp "${REPO_ROOT}/third_party/licenses/BUNGEE-NOTICE.md" \
       "${target_dir}/LICENSES/BUNGEE-NOTICE.md"
    cp "${REPO_ROOT}/third_party/licenses/ESSENTIA-NOTICE.md" \
       "${target_dir}/LICENSES/ESSENTIA-NOTICE.md"
    cp "${REPO_ROOT}/third_party/licenses/EIGEN-NOTICE.md" \
       "${target_dir}/LICENSES/EIGEN-NOTICE.md"
    cp "${REPO_ROOT}/third_party/licenses/PFFFT-NOTICE.txt" \
       "${target_dir}/LICENSES/PFFFT-NOTICE.txt"
    cp "${REPO_ROOT}/third_party/licenses/SOUNDTOUCH-NOTICE.md" \
       "${target_dir}/LICENSES/SOUNDTOUCH-NOTICE.md"
    cp "${REPO_ROOT}/JUCE/LICENSE.md" "${target_dir}/LICENSES/JUCE-LICENSE.md"
}

copy_soundtouch_notice_if_linked() {
    local target_dir="$1"
    local bundle_path="$2"
    local plugin_binary=""
    local soundtouch_lib=""
    local soundtouch_real=""
    local soundtouch_root=""

    if [[ -d "${bundle_path}/Contents/MacOS" ]]; then
        plugin_binary="$(find "${bundle_path}/Contents/MacOS" -maxdepth 1 -type f | head -n 1)"
    fi

    [[ -n "${plugin_binary}" ]] || return 0

    soundtouch_lib="$(otool -L "${plugin_binary}" | awk '/libSoundTouch/ { print $1; exit }')"
    [[ -n "${soundtouch_lib}" ]] || return 0

    soundtouch_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "${soundtouch_lib}")"
    soundtouch_root="$(cd "$(dirname "${soundtouch_real}")/.." && pwd)"

    for candidate in \
        "${soundtouch_root}/COPYING.TXT" \
        "${soundtouch_root}/COPYING" \
        "${soundtouch_root}/share/doc/soundtouch/COPYING.TXT"; do
        if [[ -f "${candidate}" ]]; then
            cp "${candidate}" "${target_dir}/LICENSES/SoundTouch-COPYING.TXT"
            return 0
        fi
    done

    echo "SoundTouch linked in ${plugin_binary}, but its COPYING notice could not be located." >&2
    exit 1
}

package_bundle() {
    local format="$1"
    local bundle_path="$2"
    local suffix="$3"
    local package_name="mlrVST-macos-${ARCH}-${suffix}-${TIMESTAMP}"
    local package_dir="${STAGE_DIR}/${package_name}"
    local zip_path="${OUT_DIR}/${package_name}.zip"

    mkdir -p "${package_dir}"
    cp -R "${bundle_path}" "${package_dir}/"
    copy_common_notices "${package_dir}"
    copy_soundtouch_notice_if_linked "${package_dir}" "${bundle_path}"

    cat > "${package_dir}/RELEASE_MANIFEST.txt" <<EOF
Project: mlrVST
Format: ${format}
Build dir: ${BUILD_DIR}
Config: ${CONFIG}
Timestamp: ${TIMESTAMP}
EOF

    (cd "${STAGE_DIR}" && COPYFILE_DISABLE=1 zip -r -y -X "${zip_path}" "${package_name}" >/dev/null)
    echo "Created: ${zip_path}"
}

if [[ -d "${VST3_BUNDLE}" ]]; then
    package_bundle "VST3" "${VST3_BUNDLE}" "vst3"
fi

if [[ -d "${AU_BUNDLE}" ]]; then
    package_bundle "AU" "${AU_BUNDLE}" "au"
fi

echo "Release packaging complete."
