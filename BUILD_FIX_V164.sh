#!/bin/bash

# mlrVST v164 - Crash Fix Build Script
# Fixes segmentation fault on plugin load in Ableton Live 12

set -e

echo "========================================="
echo "mlrVST v164 - Crash Fix Build"
echo "========================================="
echo ""

# Check if JUCE exists
if [ ! -d "JUCE" ]; then
    echo "ERROR: JUCE not found!"
    echo ""
    echo "Please clone JUCE first:"
    echo "  git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git"
    echo ""
    echo "Or download from: https://juce.com/get-juce/"
    exit 1
fi

echo "✓ JUCE found"
echo ""

# Clean previous build
echo "Cleaning previous build..."
make clean 2>/dev/null || true
rm -rf Build
echo "✓ Clean complete"
echo ""

# Configure
echo "Configuring CMake..."
make configure
echo "✓ Configuration complete"
echo ""

# Build
echo "Building mlrVST..."
make build
echo "✓ Build complete"
echo ""

# Install
echo "Installing plugins..."
sudo make install
echo "✓ Installation complete"
echo ""

echo "========================================="
echo "Build successful!"
echo "========================================="
echo ""
echo "The following plugins have been installed:"
echo "  - VST3: ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3"
echo "  - AU:   ~/Library/Audio/Plug-Ins/Components/mlrVST.component"
echo ""
echo "Changes in v164:"
echo "  - Fixed segmentation fault on plugin load"
echo "  - Added error handling in getStateInformation()"
echo "  - Added ValueTree validation before XML creation"
echo ""
echo "Please restart Ableton Live and rescan plugins."
echo ""
