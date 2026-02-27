# mlrVST Modern Edition - Makefile
# Supports macOS, Linux, and Windows (MSVC toolchain on Windows)

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(OS),Windows_NT)
    DETECTED_OS := Windows
else
    DETECTED_OS := $(UNAME_S)
endif

# Project settings
PROJECT_NAME = mlrVST
BUILD_DIR = Build
SOURCE_DIR = Source
JUCE_DIR = JUCE

# Build configuration
CONFIG ?= Release
VERBOSE ?= 0

# Detect number of cores for parallel builds
ifeq ($(DETECTED_OS),Darwin)
    NPROC := $(shell sysctl -n hw.ncpu)
else ifeq ($(DETECTED_OS),Linux)
    NPROC := $(shell nproc)
else
    NPROC := 4
endif

# CMake settings
CMAKE := cmake
CMAKE_GENERATOR ?= "Unix Makefiles"
CMAKE_BUILD_TYPE := $(CONFIG)

# Colors for output
NO_COLOR=\033[0m
GREEN=\033[0;32m
YELLOW=\033[1;33m
RED=\033[0;31m
BLUE=\033[0;34m

.PHONY: all clean configure build install help check-juce vst3 au standalone package-release sign-notarize

# Default target
all: check-juce configure build

# Help target
help:
	@echo "$(BLUE)mlrVST Modern Edition - Build System$(NO_COLOR)"
	@echo ""
	@echo "$(GREEN)Available targets:$(NO_COLOR)"
	@echo "  make               - Build everything (default)"
	@echo "  make configure     - Configure CMake"
	@echo "  make build         - Build the plugin"
	@echo "  make vst3          - Build VST3 only"
	@echo "  make au            - Build Audio Unit only (macOS)"
	@echo "  make standalone    - Build standalone app only"
	@echo "  make install       - Install plugins to system"
	@echo "  make package-release - Build + package release zips with notices (macOS)"
	@echo "  make sign-notarize - Sign + notarize VST3/AU (macOS)"
	@echo "  make clean         - Clean build directory"
	@echo "  make distclean     - Remove all build artifacts"
	@echo "  make check-juce    - Check if JUCE is available"
	@echo "  make help          - Show this help"
	@echo ""
	@echo "$(GREEN)Configuration:$(NO_COLOR)"
	@echo "  CONFIG=Debug       - Build debug version"
	@echo "  CONFIG=Release     - Build release version (default)"
	@echo "  VERBOSE=1          - Show detailed build output"
	@echo "  SIGNING_IDENTITY=...  - Developer ID Application identity"
	@echo "  NOTARY_PROFILE=...    - notarytool keychain profile"
	@echo ""
	@echo "$(GREEN)Examples:$(NO_COLOR)"
	@echo "  make CONFIG=Debug VERBOSE=1"
	@echo "  make vst3"
	@echo "  make clean && make"

# Check if JUCE exists
check-juce:
	@echo "$(BLUE)Checking for JUCE...$(NO_COLOR)"
	@if [ ! -d "$(JUCE_DIR)" ]; then \
		echo "$(RED)ERROR: JUCE not found!$(NO_COLOR)"; \
		echo "$(YELLOW)Please run: git clone https://github.com/juce-framework/JUCE.git$(NO_COLOR)"; \
		echo "$(YELLOW)Or download from: https://juce.com/get-juce/$(NO_COLOR)"; \
		exit 1; \
	else \
		echo "$(GREEN)✓ JUCE found$(NO_COLOR)"; \
	fi

# Configure CMake
configure: check-juce
	@echo "$(BLUE)Configuring CMake ($(CONFIG))...$(NO_COLOR)"
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		-G $(CMAKE_GENERATOR)
	@echo "$(GREEN)✓ Configuration complete$(NO_COLOR)"

# Build all targets
build:
	@echo "$(BLUE)Building $(PROJECT_NAME) ($(CONFIG))...$(NO_COLOR)"
	@if [ ! -d "$(BUILD_DIR)" ]; then \
		echo "$(YELLOW)Build directory not found, running configure...$(NO_COLOR)"; \
		$(MAKE) configure; \
	fi
	@cd $(BUILD_DIR) && $(CMAKE) --build . --config $(CONFIG) -j$(NPROC) $(if $(filter 1,$(VERBOSE)),--verbose,)
	@echo "$(GREEN)✓ Build complete$(NO_COLOR)"
	@$(MAKE) show-artifacts

# Build VST3 only
vst3:
	@echo "$(BLUE)Building VST3...$(NO_COLOR)"
	@cd $(BUILD_DIR) && $(CMAKE) --build . --target $(PROJECT_NAME)_VST3 --config $(CONFIG) -j$(NPROC)
	@echo "$(GREEN)✓ VST3 build complete$(NO_COLOR)"

# Build Audio Unit (macOS only)
au:
ifeq ($(DETECTED_OS),Darwin)
	@echo "$(BLUE)Building Audio Unit...$(NO_COLOR)"
	@cd $(BUILD_DIR) && $(CMAKE) --build . --target $(PROJECT_NAME)_AU --config $(CONFIG) -j$(NPROC)
	@echo "$(GREEN)✓ AU build complete$(NO_COLOR)"
else
	@echo "$(YELLOW)Audio Units are only available on macOS$(NO_COLOR)"
endif

# Build standalone app
standalone:
	@echo "$(BLUE)Building Standalone...$(NO_COLOR)"
	@cd $(BUILD_DIR) && $(CMAKE) --build . --target $(PROJECT_NAME)_Standalone --config $(CONFIG) -j$(NPROC)
	@echo "$(GREEN)✓ Standalone build complete$(NO_COLOR)"

# Install plugins
install: build
	@echo "$(BLUE)Installing plugins...$(NO_COLOR)"
ifeq ($(DETECTED_OS),Darwin)
	@echo "$(GREEN)Installing to macOS plugin directories...$(NO_COLOR)"
	@if [ -d "$(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/VST3" ]; then \
		mkdir -p ~/Library/Audio/Plug-Ins/VST3; \
		cp -r $(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/VST3/*.vst3 ~/Library/Audio/Plug-Ins/VST3/; \
		echo "$(GREEN)✓ VST3 installed$(NO_COLOR)"; \
	fi
	@if [ -d "$(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/AU" ]; then \
		mkdir -p ~/Library/Audio/Plug-Ins/Components; \
		cp -r $(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/AU/*.component ~/Library/Audio/Plug-Ins/Components/; \
		echo "$(GREEN)✓ AU installed$(NO_COLOR)"; \
	fi
else ifeq ($(DETECTED_OS),Linux)
	@echo "$(GREEN)Installing to Linux plugin directories...$(NO_COLOR)"
	@if [ -d "$(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/VST3" ]; then \
		mkdir -p ~/.vst3; \
		cp -r $(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/VST3/*.vst3 ~/.vst3/; \
		echo "$(GREEN)✓ VST3 installed$(NO_COLOR)"; \
	fi
else
	@echo "$(YELLOW)Manual installation required on Windows$(NO_COLOR)"
	@echo "$(YELLOW)Copy from: $(BUILD_DIR)/$(PROJECT_NAME)_artefacts/$(CONFIG)/$(NO_COLOR)"
endif
	@echo "$(GREEN)✓ Installation complete$(NO_COLOR)"

# Build and package release archives with license notices (macOS)
package-release: build
ifeq ($(DETECTED_OS),Darwin)
	@echo "$(BLUE)Packaging release zips...$(NO_COLOR)"
	@./scripts/package_release_macos.sh --build-dir $(BUILD_DIR) --config $(CONFIG)
	@echo "$(GREEN)✓ Release packages created in ./release$(NO_COLOR)"
else
	@echo "$(YELLOW)Release packaging script currently supports macOS only$(NO_COLOR)"
endif

# Sign + notarize plugin bundles for distribution (macOS)
sign-notarize: build
ifeq ($(DETECTED_OS),Darwin)
	@if [ -z "$(SIGNING_IDENTITY)" ]; then \
		echo "$(RED)ERROR: SIGNING_IDENTITY is required$(NO_COLOR)"; \
		echo "$(YELLOW)Example: make sign-notarize SIGNING_IDENTITY='Developer ID Application: Your Name (TEAMID)' NOTARY_PROFILE='mlrvst-notary'$(NO_COLOR)"; \
		exit 1; \
	fi
	@echo "$(BLUE)Signing and notarizing release bundles...$(NO_COLOR)"
	@SIGNING_IDENTITY="$(SIGNING_IDENTITY)" \
	 NOTARY_PROFILE="$(NOTARY_PROFILE)" \
	 APPLE_ID="$(APPLE_ID)" \
	 APPLE_APP_PASSWORD="$(APPLE_APP_PASSWORD)" \
	 TEAM_ID="$(TEAM_ID)" \
	 ./scripts/sign_and_notarize_macos.sh --build-dir $(BUILD_DIR) --config $(CONFIG)
	@echo "$(GREEN)✓ Signing/notarization complete$(NO_COLOR)"
else
	@echo "$(YELLOW)Signing/notarization script currently supports macOS only$(NO_COLOR)"
endif

# Clean build directory
clean:
	@echo "$(BLUE)Cleaning build directory...$(NO_COLOR)"
	@if [ -d "$(BUILD_DIR)" ]; then \
		cd $(BUILD_DIR) && $(CMAKE) --build . --target clean; \
		echo "$(GREEN)✓ Clean complete$(NO_COLOR)"; \
	else \
		echo "$(YELLOW)Build directory doesn't exist$(NO_COLOR)"; \
	fi

# Remove all build artifacts
distclean:
	@echo "$(BLUE)Removing all build artifacts...$(NO_COLOR)"
	@rm -rf $(BUILD_DIR)
	@echo "$(GREEN)✓ Distclean complete$(NO_COLOR)"

# Show build artifacts
show-artifacts:
	@echo ""
	@echo "$(BLUE)Build Artifacts:$(NO_COLOR)"
	@if [ -d "$(BUILD_DIR)/$(PROJECT_NAME)_artefacts" ]; then \
		find $(BUILD_DIR)/$(PROJECT_NAME)_artefacts -name "*.vst3" -o -name "*.component" -o -name "$(PROJECT_NAME)" -o -name "$(PROJECT_NAME).exe" | while read file; do \
			echo "  $(GREEN)$$file$(NO_COLOR)"; \
		done; \
	fi
	@echo ""
	@echo "$(YELLOW)Run 'make install' to install plugins to system directories$(NO_COLOR)"
