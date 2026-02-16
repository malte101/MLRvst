#!/bin/bash

echo "=== COMPLETE CLEAN - Removing ALL build artifacts and caches ==="

# Stop if anything fails
set -e

# Remove build directories
echo "Removing build directories..."
rm -rf build/
rm -rf Builds/
rm -rf cmake-build-*
rm -rf .cmake/

# Remove CLion caches
echo "Removing CLion caches..."
rm -rf .idea/
rm -rf cmake-build-debug/
rm -rf cmake-build-release/

# Remove JUCE build artifacts
echo "Removing JUCE artifacts..."
rm -rf JuceLibraryCode/

# Remove any .o files
echo "Removing object files..."
find . -name "*.o" -delete
find . -name "*.a" -delete
find . -name "*.so" -delete
find . -name "*.dylib" -delete
find . -name "*.vst3" -type d -exec rm -rf {} + 2>/dev/null || true

# Remove Makefile build artifacts
echo "Removing Makefile artifacts..."
rm -rf mlrVST_artefacts/

# Remove any cached VST3 plugins from system
echo "Removing system VST3 caches..."
rm -rf ~/.vst3/ 2>/dev/null || true
rm -rf ~/Library/Audio/Plug-Ins/VST3/mlrVST.vst3 2>/dev/null || true
rm -rf /Library/Audio/Plug-Ins/VST3/mlrVST.vst3 2>/dev/null || true

# Clear CMake cache
echo "Removing CMake cache..."
rm -f CMakeCache.txt
rm -rf CMakeFiles/

echo ""
echo "=== CLEAN COMPLETE ==="
echo ""
echo "Now run:"
echo "  make clean"
echo "  make"
echo ""
echo "Or if using CLion:"
echo "  1. File -> Invalidate Caches / Restart"
echo "  2. Build -> Clean"
echo "  3. Build -> Rebuild Project"
