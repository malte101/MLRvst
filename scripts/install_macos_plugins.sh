#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Install mlrVST plugin bundles on macOS.

Usage:
  scripts/install_macos_plugins.sh [options]

Options:
  --build-dir <path>   Build directory that contains mlrVST_artefacts/Release
                       (default: cmake-build-release)
  --system             Install to /Library/Audio/Plug-Ins/* (default)
  --user               Install to ~/Library/Audio/Plug-Ins/*
  --vst3-only          Install only VST3 bundle
  --au-only            Install only AU bundle
  --no-sudo            Do not elevate with sudo for --system installs
  -h, --help           Show this help
EOF
}

SCOPE="system"
BUILD_DIR="cmake-build-release"
INSTALL_VST3=1
INSTALL_AU=1
NO_SUDO=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --system)
            SCOPE="system"
            shift
            ;;
        --user)
            SCOPE="user"
            shift
            ;;
        --vst3-only)
            INSTALL_VST3=1
            INSTALL_AU=0
            shift
            ;;
        --au-only)
            INSTALL_VST3=0
            INSTALL_AU=1
            shift
            ;;
        --no-sudo)
            NO_SUDO=1
            shift
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

if [[ "${INSTALL_VST3}" -eq 0 && "${INSTALL_AU}" -eq 0 ]]; then
    echo "Nothing selected to install." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ "${BUILD_DIR}" = /* ]]; then
    BUILD_ROOT="${BUILD_DIR}"
else
    BUILD_ROOT="${ROOT_DIR}/${BUILD_DIR}"
fi

ARTEFACTS_DIR="${BUILD_ROOT}/mlrVST_artefacts/Release"
VST3_SRC="${ARTEFACTS_DIR}/VST3/mlrVST.vst3"
AU_SRC="${ARTEFACTS_DIR}/AU/mlrVST.component"

if [[ "${INSTALL_VST3}" -eq 1 && ! -d "${VST3_SRC}" ]]; then
    echo "VST3 bundle not found: ${VST3_SRC}" >&2
    exit 1
fi
if [[ "${INSTALL_AU}" -eq 1 && ! -d "${AU_SRC}" ]]; then
    echo "AU bundle not found: ${AU_SRC}" >&2
    exit 1
fi

if [[ "${SCOPE}" == "system" ]]; then
    VST3_DEST_DIR="/Library/Audio/Plug-Ins/VST3"
    AU_DEST_DIR="/Library/Audio/Plug-Ins/Components"
else
    VST3_DEST_DIR="${HOME}/Library/Audio/Plug-Ins/VST3"
    AU_DEST_DIR="${HOME}/Library/Audio/Plug-Ins/Components"
fi

run_install_cmd() {
    if "$@"; then
        return 0
    fi

    if [[ "${SCOPE}" == "system" && "${EUID}" -ne 0 && "${NO_SUDO}" -eq 0 ]]; then
        echo "Retrying with sudo: $*" >&2
        sudo "$@"
        return 0
    fi

    return 1
}

if [[ "${INSTALL_VST3}" -eq 1 ]]; then
    run_install_cmd mkdir -p "${VST3_DEST_DIR}"
    run_install_cmd ditto "${VST3_SRC}" "${VST3_DEST_DIR}/mlrVST.vst3"
    echo "Installed VST3 -> ${VST3_DEST_DIR}/mlrVST.vst3"
fi

if [[ "${INSTALL_AU}" -eq 1 ]]; then
    run_install_cmd mkdir -p "${AU_DEST_DIR}"
    run_install_cmd ditto "${AU_SRC}" "${AU_DEST_DIR}/mlrVST.component"
    echo "Installed AU   -> ${AU_DEST_DIR}/mlrVST.component"
fi
