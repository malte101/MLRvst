#!/bin/bash

# mlrVST v167 - Simple Build Script for macOS
# This script builds mlrVST using Xcode directly without CMake

set -e

echo "========================================="
echo "mlrVST v167 - Quick Build"
echo "========================================="
echo ""

# Check if we're on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "ERROR: This script is for macOS only"
    exit 1
fi

# Check for Xcode
if ! command -v xcodebuild &> /dev/null; then
    echo "ERROR: Xcode command line tools not found"
    echo "Please run: xcode-select --install"
    exit 1
fi

# Get JUCE if not present
if [ ! -d "JUCE" ]; then
    echo "Downloading JUCE..."
    if command -v git &> /dev/null; then
        git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git
    else
        echo "ERROR: git not found and JUCE not present"
        echo "Please install git or manually download JUCE 8.0.4 to this directory"
        exit 1
    fi
    echo "✓ JUCE downloaded"
    echo ""
fi

# Use Projucer to generate Xcode project
PROJUCER="JUCE/extras/Projucer/Builds/MacOSX/build/Release/Projucer.app/Contents/MacOS/Projucer"

if [ ! -f "$PROJUCER" ]; then
    echo "Building Projucer..."
    cd JUCE/extras/Projucer/Builds/MacOSX
    xcodebuild -configuration Release
    cd ../../../../../
    echo "✓ Projucer built"
    echo ""
fi

# Generate Xcode project
echo "Generating Xcode project..."
$PROJUCER --resave mlrVST.jucer
echo "✓ Project generated"
echo ""

# Build with Xcode
echo "Building mlrVST..."
cd Builds/MacOSX
xcodebuild -configuration Release

echo ""
echo "========================================="
echo "Build complete!"
echo "========================================="
echo ""
echo "Plugins built:"
echo "  VST3: Builds/MacOSX/build/Release/mlrVST.vst3"
echo "  AU:   Builds/MacOSX/build/Release/mlrVST.component"
echo ""
echo "To install, run:"
echo "  sudo cp -r Builds/MacOSX/build/Release/mlrVST.vst3 ~/Library/Audio/Plug-Ins/VST3/"
echo "  sudo cp -r Builds/MacOSX/build/Release/mlrVST.component ~/Library/Audio/Plug-Ins/Components/"
echo ""
