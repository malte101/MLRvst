#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_DIR="${ROOT_DIR}/third_party/_native"
ESSENTIA_PREFIX="${NATIVE_DIR}/essentia-prefix"
BUNGEE_PREFIX="${NATIVE_DIR}/bungee-prefix"

WORK_DIR="${TMPDIR:-/tmp}/mlrvst-native-build"
ESSENTIA_SRC="${WORK_DIR}/essentia-src"
BUNGEE_SRC="${WORK_DIR}/bungee-src"
BUNGEE_BUILD="${WORK_DIR}/bungee-build"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "${NATIVE_DIR}" "${WORK_DIR}"

if [[ ! -d "${ESSENTIA_SRC}/.git" ]]; then
  rm -rf "${ESSENTIA_SRC}"
  git clone https://github.com/MTG/essentia.git "${ESSENTIA_SRC}"
fi

if [[ ! -d "${BUNGEE_SRC}/.git" ]]; then
  rm -rf "${BUNGEE_SRC}"
  git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee "${BUNGEE_SRC}"
fi

rm -rf "${ESSENTIA_PREFIX}"
mkdir -p "${ESSENTIA_PREFIX}"
(
  cd "${ESSENTIA_SRC}"
  python3 waf configure --build-static --mode=release --std=c++17 --lightweight= --fft=ACCELERATE --prefix="${ESSENTIA_PREFIX}"
  python3 waf -j"${JOBS}"
  python3 waf install
)

rm -rf "${BUNGEE_BUILD}" "${BUNGEE_PREFIX}"
cmake -S "${BUNGEE_SRC}" -B "${BUNGEE_BUILD}" -DCMAKE_BUILD_TYPE=Release -DBUNGEE_BUILD_SHARED_LIBRARY=OFF
cmake --build "${BUNGEE_BUILD}" --parallel "${JOBS}"

mkdir -p "${BUNGEE_PREFIX}/include" "${BUNGEE_PREFIX}/lib"
ditto "${BUNGEE_SRC}/bungee" "${BUNGEE_PREFIX}/include/bungee"
cp "${BUNGEE_BUILD}/libbungee.a" "${BUNGEE_BUILD}/libpffft.a" "${BUNGEE_PREFIX}/lib/"

echo "Bootstrapped native dependencies into:"
echo "  ${ESSENTIA_PREFIX}"
echo "  ${BUNGEE_PREFIX}"
