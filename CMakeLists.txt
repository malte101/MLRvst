cmake_minimum_required(VERSION 3.22)

# Set C and C++ compilers (JUCE needs both)
project(mlrVST VERSION 2.0.0 LANGUAGES C CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Licensing-sensitive optional model:
# OFF by default so release binaries do not accidentally include Huovilainen code.
option(MLRVST_ENABLE_HUOVILAINEN "Enable Huovilainen Moog model (LGPL-derived upstream)" OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Check if JUCE exists
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR 
        "JUCE not found!\n"
        "Please clone JUCE:\n"
        "  cd ${CMAKE_CURRENT_SOURCE_DIR}\n"
        "  git clone https://github.com/juce-framework/JUCE.git\n")
endif()

# Add JUCE
add_subdirectory(JUCE)

# Plugin configuration
juce_add_plugin(mlrVST
    COMPANY_NAME "mlrVST"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    
    PLUGIN_MANUFACTURER_CODE Mlrx
    PLUGIN_CODE Mlrv
    
    FORMATS VST3 AU
    
    PRODUCT_NAME "mlrVST"
    
    VST3_CATEGORIES Fx Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    
    # Bundle identifiers for macOS
    BUNDLE_ID "com.mlrVST.plugin"
    
    # Description
    DESCRIPTION "Modern mlr sample slicer with Monome support"
    
    # Plugin channel configurations
    PLUGIN_CHANNEL_CONFIGURATIONS {2, 2}  # Stereo in, stereo out
    
    # Keep build outputs in the local artefacts directory.
    # Install/copy to system plugin folders is a separate explicit step.
)

# Source files
target_sources(mlrVST
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/AudioEngine.cpp
        Source/AudioEngine.h
        Source/PresetStore.cpp
        Source/PresetStore.h
        Source/MonomeController.cpp
        Source/MonomeFilterActions.cpp
        Source/MonomeFilterActions.h
        Source/MonomeGroupAssignActions.cpp
        Source/MonomeGroupAssignActions.h
        Source/MonomeMixActions.cpp
        Source/MonomeMixActions.h
        Source/MonomeFileBrowserActions.cpp
        Source/MonomeFileBrowserActions.h
)

# Compile definitions
target_compile_definitions(mlrVST
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

if(MLRVST_ENABLE_HUOVILAINEN)
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_HUOVILAINEN=1)
else()
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_HUOVILAINEN=0)
endif()

# Link libraries
target_link_libraries(mlrVST
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(mlrVST PRIVATE JUCE_MAC=1)
    
    # Code signing (optional)
    # set_target_properties(mlrVST_VST3 PROPERTIES
    #     XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Developer ID Application"
    #     XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "YOUR_TEAM_ID"
    # )
    
elseif(WIN32)
    target_compile_definitions(mlrVST PRIVATE JUCE_WINDOWS=1)
    
elseif(UNIX)
    target_compile_definitions(mlrVST PRIVATE JUCE_LINUX=1)
    
    # Linux-specific packages
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT webkit2gtk-4.0)
    pkg_check_modules(GTK gtk+-3.0)
    
    if(WEBKIT_FOUND)
        target_link_libraries(mlrVST PRIVATE ${WEBKIT_LIBRARIES})
    endif()
    
    if(GTK_FOUND)
        target_link_libraries(mlrVST PRIVATE ${GTK_LIBRARIES})
    endif()
endif()

# Optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(mlrVST PRIVATE /O2 /GL)
    else()
        target_compile_options(mlrVST PRIVATE -O3 -ffast-math)
    endif()
endif()

# Debug flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(mlrVST PRIVATE DEBUG=1 _DEBUG=1)
    
    if(NOT MSVC)
        target_compile_options(mlrVST PRIVATE -g -Wall -Wextra)
    endif()
endif()

# Reduce known warning noise from JUCE + external DSP headers when building with Clang.
# This does not change DSP/runtime behavior.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mlrVST PRIVATE
        -Wno-nan-infinity-disabled
    )
endif()

# Include directories
target_include_directories(mlrVST
    PRIVATE
        Source
)

# Third-party headers are treated as system includes to avoid surfacing
# vendor warnings as project warnings.
target_include_directories(mlrVST
    SYSTEM PRIVATE
        third_party/MoogLadders-main/src
)

# Installation
install(TARGETS mlrVST
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

if(APPLE)
    set(MLRVST_MAC_INSTALL_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_macos_plugins.sh")
    if(EXISTS "${MLRVST_MAC_INSTALL_SCRIPT}")
        add_custom_target(install_plugins_user
            COMMAND "${MLRVST_MAC_INSTALL_SCRIPT}" --user --build-dir "${CMAKE_BINARY_DIR}"
            USES_TERMINAL
            COMMENT "Install mlrVST plugin bundles to user plugin folders")

        add_custom_target(install_plugins_system
            COMMAND "${MLRVST_MAC_INSTALL_SCRIPT}" --system --build-dir "${CMAKE_BINARY_DIR}"
            USES_TERMINAL
            COMMENT "Install mlrVST plugin bundles to system plugin folders (sudo may prompt)")
    endif()
endif()

# Print configuration
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "mlrVST v${PROJECT_VERSION} - Build Configuration")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Huovilainen model: ${MLRVST_ENABLE_HUOVILAINEN}")
message(STATUS "")
message(STATUS "Plugin Formats:")
message(STATUS "  ✓ VST3")
message(STATUS "  ✓ AU (Audio Unit)")
message(STATUS "")
if(APPLE)
    message(STATUS "macOS build outputs:")
    message(STATUS "  VST3: ${CMAKE_BINARY_DIR}/mlrVST_artefacts/Release/VST3/mlrVST.vst3")
    message(STATUS "  AU:   ${CMAKE_BINARY_DIR}/mlrVST_artefacts/Release/AU/mlrVST.component")
    message(STATUS "")
    message(STATUS "To install plugin bundles:")
    message(STATUS "  cmake --build ${CMAKE_BINARY_DIR} --target install_plugins_user")
    message(STATUS "  cmake --build ${CMAKE_BINARY_DIR} --target install_plugins_system")
    message(STATUS "  # or")
    message(STATUS "  scripts/install_macos_plugins.sh --system --build-dir ${CMAKE_BINARY_DIR}")
    message(STATUS "")
    message(STATUS "Build artifacts will be in:")
    message(STATUS "  ${CMAKE_BINARY_DIR}/mlrVST_artefacts/")
endif()
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
