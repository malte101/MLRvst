#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./scripts/sign_and_notarize_macos.sh [options]

Signs and notarizes mlrVST plugin bundles (VST3/AU) for macOS distribution.

Options:
  --build-dir DIR         Build directory containing mlrVST_artefacts (default: auto-detect)
  --config CONFIG         Build config folder (default: Release)
  --identity ID           Developer ID Application identity
  --notary-profile NAME   notarytool keychain profile name
  --apple-id EMAIL        Apple ID (if not using --notary-profile)
  --app-password PASS     App-specific password (if not using --notary-profile)
  --team-id TEAMID        Apple team ID (if not using --notary-profile)
  --out-dir DIR           Output directory for distributable zip files
  --skip-notarize         Only sign + verify (no notarization/stapling)
  --skip-staple           Notarize but do not staple
  --skip-vst3             Skip VST3 bundle
  --skip-au               Skip AU bundle
  -h, --help              Show this help

Environment fallbacks:
  SIGNING_IDENTITY, NOTARY_PROFILE, APPLE_ID, APPLE_APP_PASSWORD, TEAM_ID

Examples:
  ./scripts/sign_and_notarize_macos.sh \
      --build-dir cmake-build-release \
      --config Release \
      --identity "Developer ID Application: Your Name (TEAMID)" \
      --notary-profile "mlrvst-notary"

  APPLE_ID="name@example.com" APPLE_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx" TEAM_ID="ABCDE12345" \
  ./scripts/sign_and_notarize_macos.sh --identity "Developer ID Application: Your Name (ABCDE12345)"
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR=""
CONFIG="Release"
IDENTITY="${SIGNING_IDENTITY:-${IDENTITY:-}}"
NOTARY_PROFILE="${NOTARY_PROFILE:-${NOTARY_KEYCHAIN_PROFILE:-}}"
APPLE_ID="${APPLE_ID:-}"
APPLE_APP_PASSWORD="${APPLE_APP_PASSWORD:-}"
TEAM_ID="${TEAM_ID:-}"
OUT_DIR=""
SKIP_NOTARIZE=0
SKIP_STAPLE=0
SKIP_VST3=0
SKIP_AU=0

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
        --identity)
            IDENTITY="${2:-}"
            shift 2
            ;;
        --notary-profile)
            NOTARY_PROFILE="${2:-}"
            shift 2
            ;;
        --apple-id)
            APPLE_ID="${2:-}"
            shift 2
            ;;
        --app-password)
            APPLE_APP_PASSWORD="${2:-}"
            shift 2
            ;;
        --team-id)
            TEAM_ID="${2:-}"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="${2:-}"
            shift 2
            ;;
        --skip-notarize)
            SKIP_NOTARIZE=1
            shift
            ;;
        --skip-staple)
            SKIP_STAPLE=1
            shift
            ;;
        --skip-vst3)
            SKIP_VST3=1
            shift
            ;;
        --skip-au)
            SKIP_AU=1
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

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script supports macOS only." >&2
    exit 1
fi

require_cmd codesign
require_cmd xcrun
require_cmd ditto

if [[ -z "${IDENTITY}" ]]; then
    echo "Missing signing identity. Use --identity or SIGNING_IDENTITY env var." >&2
    exit 1
fi

if [[ "${SKIP_NOTARIZE}" -eq 0 ]]; then
    if [[ -z "${NOTARY_PROFILE}" ]]; then
        if [[ -z "${APPLE_ID}" || -z "${APPLE_APP_PASSWORD}" || -z "${TEAM_ID}" ]]; then
            echo "Notarization requires either:" >&2
            echo "  --notary-profile <profile>" >&2
            echo "or all of:" >&2
            echo "  --apple-id <email> --app-password <password> --team-id <team>" >&2
            exit 1
        fi
    fi
fi

if [[ -z "${BUILD_DIR}" ]]; then
    if [[ -d "${REPO_ROOT}/cmake-build-release/mlrVST_artefacts/${CONFIG}" ]]; then
        BUILD_DIR="cmake-build-release"
    elif [[ -d "${REPO_ROOT}/Build/mlrVST_artefacts/${CONFIG}" ]]; then
        BUILD_DIR="Build"
    else
        echo "Could not auto-detect build dir. Pass --build-dir explicitly." >&2
        exit 1
    fi
fi

ARTEFACTS_DIR="${REPO_ROOT}/${BUILD_DIR}/mlrVST_artefacts/${CONFIG}"
VST3_BUNDLE="${ARTEFACTS_DIR}/VST3/mlrVST.vst3"
AU_BUNDLE="${ARTEFACTS_DIR}/AU/mlrVST.component"

declare -a TARGET_BUNDLES=()
declare -a TARGET_FORMATS=()

if [[ "${SKIP_VST3}" -eq 0 && -d "${VST3_BUNDLE}" ]]; then
    TARGET_BUNDLES+=("${VST3_BUNDLE}")
    TARGET_FORMATS+=("vst3")
fi

if [[ "${SKIP_AU}" -eq 0 && -d "${AU_BUNDLE}" ]]; then
    TARGET_BUNDLES+=("${AU_BUNDLE}")
    TARGET_FORMATS+=("au")
fi

if [[ "${#TARGET_BUNDLES[@]}" -eq 0 ]]; then
    echo "No target bundles found to process under: ${ARTEFACTS_DIR}" >&2
    exit 1
fi

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
ARCH="$(uname -m)"

if [[ -z "${OUT_DIR}" ]]; then
    OUT_DIR="${REPO_ROOT}/release/notarized-${TIMESTAMP}"
fi
mkdir -p "${OUT_DIR}"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mlrvst-notary-XXXXXX")"
cleanup() {
    rm -rf "${STAGE_DIR}"
}
trap cleanup EXIT

submit_for_notarization() {
    local zip_path="$1"
    if [[ -n "${NOTARY_PROFILE}" ]]; then
        xcrun notarytool submit "${zip_path}" \
            --keychain-profile "${NOTARY_PROFILE}" \
            --wait
    else
        xcrun notarytool submit "${zip_path}" \
            --apple-id "${APPLE_ID}" \
            --password "${APPLE_APP_PASSWORD}" \
            --team-id "${TEAM_ID}" \
            --wait
    fi
}

for i in "${!TARGET_BUNDLES[@]}"; do
    bundle="${TARGET_BUNDLES[$i]}"
    format="${TARGET_FORMATS[$i]}"
    format_upper="$(echo "${format}" | tr '[:lower:]' '[:upper:]')"

    echo ""
    echo "=== ${format_upper}: ${bundle}"

    xattr -dr com.apple.quarantine "${bundle}" 2>/dev/null || true

    echo "Signing..."
    codesign --force --deep --timestamp --options runtime --sign "${IDENTITY}" "${bundle}"

    echo "Verifying signature..."
    codesign --verify --deep --strict --verbose=2 "${bundle}"
    codesign -dv --verbose=4 "${bundle}" 2>&1 | sed -n '1,20p'

    if [[ "${SKIP_NOTARIZE}" -eq 0 ]]; then
        NOTARY_ZIP="${STAGE_DIR}/mlrVST-${format}-notary.zip"
        echo "Preparing notarization archive: ${NOTARY_ZIP}"
        ditto -c -k --keepParent "${bundle}" "${NOTARY_ZIP}"

        echo "Submitting for notarization..."
        submit_for_notarization "${NOTARY_ZIP}"

        if [[ "${SKIP_STAPLE}" -eq 0 ]]; then
            echo "Stapling ticket..."
            xcrun stapler staple "${bundle}"
            xcrun stapler validate "${bundle}"
        fi
    fi

    DIST_ZIP="${OUT_DIR}/mlrVST-macos-${ARCH}-${format}-${TIMESTAMP}.zip"
    echo "Creating distributable zip: ${DIST_ZIP}"
    ditto -c -k --keepParent "${bundle}" "${DIST_ZIP}"
done

echo ""
echo "Done."
echo "Signed identity: ${IDENTITY}"
echo "Output zips: ${OUT_DIR}"
if [[ "${SKIP_NOTARIZE}" -eq 1 ]]; then
    echo "Notarization was skipped."
fi
