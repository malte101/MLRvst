#!/bin/bash

echo ""
echo "═══════════════════════════════════════════════════════"
echo "mlrVST Bundle Checker"
echo "═══════════════════════════════════════════════════════"
echo ""

# Check Debug builds
echo "Debug Builds:"
if [ -d "cmake-build-debug/mlrVST_artefacts/Debug/VST3/mlrVST.vst3" ]; then
    echo "  ✓ VST3: cmake-build-debug/mlrVST_artefacts/Debug/VST3/mlrVST.vst3"
else
    echo "  ✗ VST3 not found"
fi

if [ -d "cmake-build-debug/mlrVST_artefacts/Debug/AU/mlrVST.component" ]; then
    echo "  ✓ AU:   cmake-build-debug/mlrVST_artefacts/Debug/AU/mlrVST.component"
else
    echo "  ✗ AU not found"
fi

echo ""

# Check Release builds
echo "Release Builds:"
if [ -d "cmake-build-release/mlrVST_artefacts/Release/VST3/mlrVST.vst3" ]; then
    echo "  ✓ VST3: cmake-build-release/mlrVST_artefacts/Release/VST3/mlrVST.vst3"
else
    echo "  ✗ VST3 not found"
fi

if [ -d "cmake-build-release/mlrVST_artefacts/Release/AU/mlrVST.component" ]; then
    echo "  ✓ AU:   cmake-build-release/mlrVST_artefacts/Release/AU/mlrVST.component"
else
    echo "  ✗ AU not found"
fi

echo ""

# Check system installation
echo "System Installation:"
if [ -d "$HOME/Library/Audio/Plug-Ins/VST3/mlrVST.vst3" ]; then
    echo "  ✓ VST3: ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3"
else
    echo "  ✗ VST3 not installed"
fi

if [ -d "$HOME/Library/Audio/Plug-Ins/Components/mlrVST.component" ]; then
    echo "  ✓ AU:   ~/Library/Audio/Plug-Ins/Components/mlrVST.component"
else
    echo "  ✗ AU not installed"
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo ""

# If no bundles found, show help
if [ ! -d "cmake-build-release/mlrVST_artefacts/Release/VST3/mlrVST.vst3" ] && \
   [ ! -d "cmake-build-debug/mlrVST_artefacts/Debug/VST3/mlrVST.vst3" ]; then
    echo "No bundles found!"
    echo ""
    echo "In CLion:"
    echo "  1. Select build type: [Release ▼] in top toolbar"
    echo "  2. Select target: [mlrVST_All ▼]"
    echo "  3. Build: Cmd+F9 (Mac) or Ctrl+F9 (Windows)"
    echo ""
    echo "Bundles will appear in:"
    echo "  cmake-build-release/mlrVST_artefacts/Release/"
    echo ""
fi
