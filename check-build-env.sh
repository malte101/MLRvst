#!/bin/bash

# mlrVST Compiler Check Script
# Run this to diagnose build issues

echo "======================================"
echo "mlrVST Build Environment Check"
echo "======================================"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check functions
check_command() {
    if command -v $1 &> /dev/null; then
        echo -e "${GREEN}✓${NC} $1 found: $(command -v $1)"
        if [ "$2" = "version" ]; then
            echo "  Version: $($1 --version | head -n1)"
        fi
        return 0
    else
        echo -e "${RED}✗${NC} $1 not found"
        return 1
    fi
}

# 1. Check CMake
echo "1. Checking CMake..."
if check_command cmake version; then
    CMAKE_VERSION=$(cmake --version | head -n1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    REQUIRED_VERSION="3.22"
    if [ "$(printf '%s\n' "$REQUIRED_VERSION" "$CMAKE_VERSION" | sort -V | head -n1)" = "$REQUIRED_VERSION" ]; then
        echo -e "  ${GREEN}✓ Version OK (>= 3.22)${NC}"
    else
        echo -e "  ${YELLOW}⚠ Version too old. Need >= 3.22${NC}"
    fi
else
    echo -e "  ${RED}INSTALL: brew install cmake (macOS) or apt-get install cmake (Linux)${NC}"
fi
echo ""

# 2. Check C Compiler
echo "2. Checking C Compiler..."
CC_FOUND=0
if check_command gcc version; then
    CC_FOUND=1
    export CC=$(command -v gcc)
fi
if check_command clang version; then
    CC_FOUND=1
    if [ -z "$CC" ]; then
        export CC=$(command -v clang)
    fi
fi
if [ $CC_FOUND -eq 0 ]; then
    echo -e "  ${RED}INSTALL: xcode-select --install (macOS) or apt-get install build-essential (Linux)${NC}"
fi
echo ""

# 3. Check C++ Compiler
echo "3. Checking C++ Compiler..."
CXX_FOUND=0
if check_command g++ version; then
    CXX_FOUND=1
    export CXX=$(command -v g++)
fi
if check_command clang++ version; then
    CXX_FOUND=1
    if [ -z "$CXX" ]; then
        export CXX=$(command -v clang++)
    fi
fi
if [ $CXX_FOUND -eq 0 ]; then
    echo -e "  ${RED}INSTALL: xcode-select --install (macOS) or apt-get install build-essential (Linux)${NC}"
fi
echo ""

# 4. Check Make
echo "4. Checking Make..."
check_command make version
echo ""

# 5. Check Git
echo "5. Checking Git..."
check_command git version
echo ""

# 6. Check JUCE
echo "6. Checking JUCE..."
if [ -d "JUCE" ]; then
    if [ -f "JUCE/CMakeLists.txt" ]; then
        echo -e "${GREEN}✓${NC} JUCE found and valid"
        if [ -d "JUCE/.git" ]; then
            cd JUCE
            JUCE_VERSION=$(git describe --tags 2>/dev/null || echo "unknown")
            echo "  Version/Tag: $JUCE_VERSION"
            cd ..
        fi
    else
        echo -e "${YELLOW}⚠${NC} JUCE directory exists but CMakeLists.txt missing"
        echo -e "  ${YELLOW}TRY: rm -rf JUCE && git clone https://github.com/juce-framework/JUCE.git${NC}"
    fi
else
    echo -e "${RED}✗${NC} JUCE not found"
    echo -e "  ${RED}RUN: git clone https://github.com/juce-framework/JUCE.git${NC}"
fi
echo ""

# 7. Platform-specific checks
echo "7. Platform-Specific Requirements..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Platform: macOS"
    if xcode-select -p &> /dev/null; then
        echo -e "${GREEN}✓${NC} Xcode Command Line Tools installed"
    else
        echo -e "${RED}✗${NC} Xcode Command Line Tools not installed"
        echo -e "  ${RED}RUN: xcode-select --install${NC}"
    fi
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "Platform: Linux"
    echo "Checking development packages..."
    
    PACKAGES=(libasound2-dev libx11-dev libfreetype6-dev libjack-jackd2-dev)
    for pkg in "${PACKAGES[@]}"; do
        if dpkg -l | grep -q "^ii  $pkg"; then
            echo -e "${GREEN}✓${NC} $pkg"
        else
            echo -e "${YELLOW}⚠${NC} $pkg not installed"
            echo -e "  ${YELLOW}RUN: sudo apt-get install $pkg${NC}"
        fi
    done
else
    echo "Platform: Windows or Other"
    echo "Make sure you have Visual Studio 2019+ or MinGW installed"
fi
echo ""

# 8. Test CMake Configuration
echo "8. Testing CMake Configuration..."
if [ -d "JUCE" ] && [ $CC_FOUND -eq 1 ] && [ $CXX_FOUND -eq 1 ]; then
    echo "Attempting test configuration..."
    mkdir -p Build-test
    cd Build-test
    
    if cmake .. -DCMAKE_BUILD_TYPE=Debug &> /dev/null; then
        echo -e "${GREEN}✓${NC} CMake configuration successful!"
        cd ..
        rm -rf Build-test
    else
        echo -e "${RED}✗${NC} CMake configuration failed"
        echo "Last few lines of error:"
        cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -n 10
        cd ..
        rm -rf Build-test
    fi
else
    echo -e "${YELLOW}⚠${NC} Skipping test (missing requirements)"
fi
echo ""

# Summary
echo "======================================"
echo "Summary"
echo "======================================"
echo ""

if [ $CC_FOUND -eq 1 ] && [ $CXX_FOUND -eq 1 ] && [ -d "JUCE" ]; then
    echo -e "${GREEN}✓ Ready to build!${NC}"
    echo ""
    echo "Next steps:"
    echo "  make                    # Build everything"
    echo "  make CONFIG=Debug       # Debug build"
    echo "  make install            # Install plugins"
    echo ""
    echo "For CLion:"
    echo "  1. File → Open → Select mlrVST-modern folder"
    echo "  2. CLion will configure automatically"
    echo "  3. Build → Build Project"
else
    echo -e "${RED}✗ Not ready to build${NC}"
    echo ""
    echo "Please install missing components listed above."
    echo ""
    if [ $CC_FOUND -eq 0 ] || [ $CXX_FOUND -eq 0 ]; then
        echo "To fix compiler issues:"
        if [[ "$OSTYPE" == "darwin"* ]]; then
            echo "  xcode-select --install"
        elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
            echo "  sudo apt-get install build-essential cmake"
        fi
    fi
    if [ ! -d "JUCE" ]; then
        echo "To fix JUCE:"
        echo "  git clone https://github.com/juce-framework/JUCE.git"
    fi
fi
echo ""

# Export environment
if [ $CC_FOUND -eq 1 ] && [ $CXX_FOUND -eq 1 ]; then
    echo "Detected compilers:"
    echo "  CC=$CC"
    echo "  CXX=$CXX"
    echo ""
    echo "You can export these:"
    echo "  export CC=$CC"
    echo "  export CXX=$CXX"
fi
