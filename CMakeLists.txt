cmake_minimum_required(VERSION 3.22)

# Set C and C++ compilers (JUCE needs both)
project(mlrVST VERSION 2.0.1 LANGUAGES C CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Licensing-sensitive optional model:
# OFF by default so release binaries do not accidentally include Huovilainen code.
option(MLRVST_ENABLE_HUOVILAINEN "Enable Huovilainen Moog model (LGPL-derived upstream)" OFF)
option(MLRVST_ENABLE_SOUNDTOUCH "Enable SoundTouch-based swing time-stretch path" ON)
option(MLRVST_ENABLE_BUNGEE "Enable Bungee-based stretch backend" ON)
option(MLRVST_ENABLE_LIBPYIN "Enable vendored LibPyin monophonic pitch detection for loop analysis" ON)
set(MLRVST_NATIVE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/_native" CACHE PATH
    "Path to persistent repo-local native dependency prefixes")
set(MLRVST_DEFAULT_ESSENTIA_PREFIX "${MLRVST_NATIVE_DEPS_DIR}/essentia-prefix")
set(MLRVST_DEFAULT_BUNGEE_PREFIX "${MLRVST_NATIVE_DEPS_DIR}/bungee-prefix")
set(MLRVST_ESSENTIA_PREFIX "${MLRVST_DEFAULT_ESSENTIA_PREFIX}" CACHE PATH
    "Path to a native Essentia install prefix")
set(MLRVST_BUNGEE_PREFIX "${MLRVST_DEFAULT_BUNGEE_PREFIX}" CACHE PATH
    "Path to a persistent Bungee install prefix")
set(MLRVST_BUNGEE_SOURCE_DIR "${MLRVST_NATIVE_DEPS_DIR}/_bungee-source" CACHE PATH
    "Path to a Bungee source checkout")
set(MLRVST_BUNGEE_BUILD_DIR "${MLRVST_NATIVE_DEPS_DIR}/_bungee-build" CACHE PATH
    "Path to a built Bungee tree")

if(EXISTS "${MLRVST_DEFAULT_ESSENTIA_PREFIX}/include/essentia/algorithmfactory.h"
   AND ((NOT EXISTS "${MLRVST_ESSENTIA_PREFIX}/include/essentia/algorithmfactory.h")
        OR "${MLRVST_ESSENTIA_PREFIX}" MATCHES "^/tmp($|/)"))
    set(MLRVST_ESSENTIA_PREFIX "${MLRVST_DEFAULT_ESSENTIA_PREFIX}" CACHE PATH
        "Path to a native Essentia install prefix" FORCE)
    unset(MLRVST_ESSENTIA_INCLUDE_DIR CACHE)
    unset(MLRVST_ESSENTIA_LIBRARY CACHE)
endif()

if(EXISTS "${MLRVST_ESSENTIA_PREFIX}/include/essentia/algorithmfactory.h"
   AND EXISTS "${MLRVST_ESSENTIA_PREFIX}/lib/libessentia.a")
    set(MLRVST_ESSENTIA_INCLUDE_DIR "${MLRVST_ESSENTIA_PREFIX}/include" CACHE PATH
        "Resolved native Essentia include directory" FORCE)
    set(MLRVST_ESSENTIA_LIBRARY "${MLRVST_ESSENTIA_PREFIX}/lib/libessentia.a" CACHE FILEPATH
        "Resolved native Essentia library" FORCE)
endif()

if(EXISTS "${MLRVST_DEFAULT_BUNGEE_PREFIX}/include/bungee/Bungee.h"
   AND EXISTS "${MLRVST_DEFAULT_BUNGEE_PREFIX}/lib/libbungee.a"
   AND ((NOT EXISTS "${MLRVST_BUNGEE_PREFIX}/include/bungee/Bungee.h")
        OR "${MLRVST_BUNGEE_PREFIX}" MATCHES "^/tmp($|/)"))
    set(MLRVST_BUNGEE_PREFIX "${MLRVST_DEFAULT_BUNGEE_PREFIX}" CACHE PATH
        "Path to a persistent Bungee install prefix" FORCE)
    unset(MLRVST_BUNGEE_INCLUDE_DIR CACHE)
    unset(MLRVST_BUNGEE_LIBRARY CACHE)
    unset(MLRVST_BUNGEE_PFFFT_LIBRARY CACHE)
endif()

if(EXISTS "${MLRVST_BUNGEE_PREFIX}/include/bungee/Bungee.h"
   AND EXISTS "${MLRVST_BUNGEE_PREFIX}/lib/libbungee.a"
   AND EXISTS "${MLRVST_BUNGEE_PREFIX}/lib/libpffft.a")
    set(MLRVST_BUNGEE_INCLUDE_DIR "${MLRVST_BUNGEE_PREFIX}/include" CACHE PATH
        "Resolved native Bungee include directory" FORCE)
    set(MLRVST_BUNGEE_LIBRARY "${MLRVST_BUNGEE_PREFIX}/lib/libbungee.a" CACHE FILEPATH
        "Resolved native Bungee library" FORCE)
    set(MLRVST_BUNGEE_PFFFT_LIBRARY "${MLRVST_BUNGEE_PREFIX}/lib/libpffft.a" CACHE FILEPATH
        "Resolved native PFFFT library for Bungee" FORCE)
endif()

if("${MLRVST_ESSENTIA_INCLUDE_DIR}" MATCHES "^/tmp($|/)"
   OR "${MLRVST_ESSENTIA_LIBRARY}" MATCHES "^/tmp($|/)")
    unset(MLRVST_ESSENTIA_INCLUDE_DIR CACHE)
    unset(MLRVST_ESSENTIA_LIBRARY CACHE)
endif()

if("${MLRVST_BUNGEE_INCLUDE_DIR}" MATCHES "^/tmp($|/)"
   OR "${MLRVST_BUNGEE_LIBRARY}" MATCHES "^/tmp($|/)"
   OR "${MLRVST_BUNGEE_PFFFT_LIBRARY}" MATCHES "^/tmp($|/)")
    unset(MLRVST_BUNGEE_INCLUDE_DIR CACHE)
    unset(MLRVST_BUNGEE_LIBRARY CACHE)
    unset(MLRVST_BUNGEE_PFFFT_LIBRARY CACHE)
endif()

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# JUCE source tree. Prefer a repo-local checkout, but allow overriding from the
# command line and fall back to the sibling step-vsthost workspace on this machine.
set(MLRVST_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to the JUCE source tree")
if(NOT EXISTS "${MLRVST_JUCE_PATH}/CMakeLists.txt"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../step-vsthost/JUCE/CMakeLists.txt")
    set(MLRVST_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../step-vsthost/JUCE")
endif()
get_filename_component(MLRVST_JUCE_PATH "${MLRVST_JUCE_PATH}" ABSOLUTE)

if(NOT EXISTS "${MLRVST_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR 
        "JUCE not found!\n"
        "Set MLRVST_JUCE_PATH to a JUCE checkout, or clone JUCE locally:\n"
        "  cmake -S ${CMAKE_CURRENT_SOURCE_DIR} -B ${CMAKE_CURRENT_BINARY_DIR} -DMLRVST_JUCE_PATH=/path/to/JUCE\n"
        "  # or\n"
        "  cd ${CMAKE_CURRENT_SOURCE_DIR}\n"
        "  git clone https://github.com/juce-framework/JUCE.git\n")
endif()

# JUCE 8 does not support MinGW on Windows targets.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "MinGW is not supported by JUCE 8 for this project.\n"
        "Use Visual Studio (MSVC) or clang-cl with the Windows SDK.")
endif()

# Add JUCE
add_subdirectory("${MLRVST_JUCE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")

set(MLRVST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MLRVST_BUILD_INFO_HEADER "${MLRVST_GENERATED_DIR}/mlrvst_build_info.h")
file(MAKE_DIRECTORY "${MLRVST_GENERATED_DIR}")
file(WRITE "${MLRVST_BUILD_INFO_HEADER}"
    "#pragma once\n"
    "#define MLRVST_BUILD_VERSION \"${PROJECT_VERSION}\"\n"
    "#define MLRVST_BUILD_STAMP \"configure\"\n"
    "#define MLRVST_BUILD_STAMP_COMPACT \"configure\"\n")

add_custom_target(mlrVST_build_info ALL
    COMMAND "${CMAKE_COMMAND}"
        -DOUTPUT_FILE=${MLRVST_BUILD_INFO_HEADER}
        -DMLRVST_PROJECT_VERSION=${PROJECT_VERSION}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake"
    COMMENT "Generate mlrVST build metadata"
    VERBATIM)

# Plugin formats by platform
set(MLRVST_PLUGIN_FORMATS VST3)
set(MLRVST_AU_MAIN_TYPE_ARGS)
if(APPLE)
    list(APPEND MLRVST_PLUGIN_FORMATS AU)
    list(APPEND MLRVST_AU_MAIN_TYPE_ARGS AU_MAIN_TYPE kAudioUnitType_MusicEffect)
endif()

# Plugin configuration
juce_add_plugin(mlrVST
    COMPANY_NAME "mlrVST"
    VERSION ${PROJECT_VERSION}
    BUILD_VERSION ${PROJECT_VERSION}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    
    PLUGIN_MANUFACTURER_CODE Mlrx
    PLUGIN_CODE Mlrv
    
    FORMATS ${MLRVST_PLUGIN_FORMATS}
    
    PRODUCT_NAME "mlrVST"
    
    VST3_CATEGORIES Fx Sampler
    ${MLRVST_AU_MAIN_TYPE_ARGS}
    
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
        Source/TimeStretchBackend.cpp
        Source/TimeStretchBackend.h
        Source/SampleMode.cpp
        Source/SampleMode.h
        Source/MonomeController.cpp
        Source/ArcController.cpp
        Source/MonomeFilterActions.cpp
        Source/MonomeFilterActions.h
        Source/MonomeGroupAssignActions.cpp
        Source/MonomeGroupAssignActions.h
        Source/MonomeMixActions.cpp
        Source/MonomeMixActions.h
        Source/MonomeFileBrowserActions.cpp
        Source/MonomeFileBrowserActions.h
)

set(MLRVST_LIBPYIN_ENABLED FALSE)
if(MLRVST_ENABLE_LIBPYIN
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibPyin/source/libpyincpp.h")
    target_sources(mlrVST
        PRIVATE
            third_party/LibPyin/source/libpyincpp.cpp
            third_party/LibPyin/source/normal_dist_pdf.cpp
            third_party/LibPyin/source/pyin/MonoNote.cpp
            third_party/LibPyin/source/pyin/MonoNoteHMM.cpp
            third_party/LibPyin/source/pyin/MonoNoteParameters.cpp
            third_party/LibPyin/source/pyin/MonoPitch.cpp
            third_party/LibPyin/source/pyin/MonoPitchHMM.cpp
            third_party/LibPyin/source/pyin/PYIN.cpp
            third_party/LibPyin/source/pyin/SparseHMM.cpp
            third_party/LibPyin/source/pyin/VampYin.cpp
            third_party/LibPyin/source/pyin/Yin.cpp
            third_party/LibPyin/source/pyin/YinUtil.cpp
            third_party/LibPyin/source/vamp-sdk/FFT.cpp
            third_party/LibPyin/source/vamp-sdk/FFTimpl.cpp
            third_party/LibPyin/source/vamp-sdk/PluginAdapter.cpp
            third_party/LibPyin/source/vamp-sdk/RealTime.cpp)
    target_include_directories(mlrVST SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibPyin/source"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibPyin/source/pyin"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibPyin/source/vamp-sdk"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibPyin/source/vamp")
    target_compile_definitions(mlrVST PRIVATE LIBPYIN_BUILD_SHARED=1)
    set(MLRVST_LIBPYIN_ENABLED TRUE)
endif()

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

if(MLRVST_LIBPYIN_ENABLED)
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_LIBPYIN=1)
else()
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_LIBPYIN=0)
endif()

set(MLRVST_SOUNDTOUCH_ENABLED FALSE)
if(MLRVST_ENABLE_SOUNDTOUCH)
    find_path(MLRVST_SOUNDTOUCH_INCLUDE_DIR
        NAMES soundtouch/SoundTouch.h
        PATHS /opt/homebrew/include /usr/local/include)
    find_library(MLRVST_SOUNDTOUCH_LIBRARY
        NAMES SoundTouch soundtouch
        PATHS /opt/homebrew/lib /usr/local/lib)

    if(MLRVST_SOUNDTOUCH_INCLUDE_DIR AND MLRVST_SOUNDTOUCH_LIBRARY)
        target_include_directories(mlrVST SYSTEM PRIVATE "${MLRVST_SOUNDTOUCH_INCLUDE_DIR}")
        target_link_libraries(mlrVST PRIVATE "${MLRVST_SOUNDTOUCH_LIBRARY}")
        target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_SOUNDTOUCH=1)
        set(MLRVST_SOUNDTOUCH_ENABLED TRUE)
    else()
        message(WARNING "SoundTouch requested but not found; building with legacy swing path.")
        target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_SOUNDTOUCH=0)
    endif()
else()
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_SOUNDTOUCH=0)
endif()

set(MLRVST_BUNGEE_ENABLED FALSE)
if(MLRVST_ENABLE_BUNGEE)
    find_path(MLRVST_BUNGEE_INCLUDE_DIR
        NAMES bungee/Bungee.h
        PATHS
            "${MLRVST_BUNGEE_PREFIX}/include"
            "${MLRVST_BUNGEE_SOURCE_DIR}"
            /opt/homebrew/include
            /usr/local/include)
    find_library(MLRVST_BUNGEE_LIBRARY
        NAMES bungee libbungee
        PATHS
            "${MLRVST_BUNGEE_PREFIX}/lib"
            "${MLRVST_BUNGEE_BUILD_DIR}"
            "${MLRVST_BUNGEE_BUILD_DIR}/Release"
            /opt/homebrew/lib
            /usr/local/lib)
    find_library(MLRVST_BUNGEE_PFFFT_LIBRARY
        NAMES pffft libpffft
        PATHS
            "${MLRVST_BUNGEE_PREFIX}/lib"
            "${MLRVST_BUNGEE_BUILD_DIR}"
            "${MLRVST_BUNGEE_BUILD_DIR}/Release")

    if(MLRVST_BUNGEE_INCLUDE_DIR AND MLRVST_BUNGEE_LIBRARY AND MLRVST_BUNGEE_PFFFT_LIBRARY)
        target_include_directories(mlrVST SYSTEM PRIVATE "${MLRVST_BUNGEE_INCLUDE_DIR}")
        target_link_libraries(mlrVST PRIVATE "${MLRVST_BUNGEE_LIBRARY}" "${MLRVST_BUNGEE_PFFFT_LIBRARY}")
        target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_BUNGEE=1)
        set(MLRVST_BUNGEE_ENABLED TRUE)
    else()
        message(WARNING "Bungee requested but not found; Bungee stretch backend will be unavailable.")
        target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_BUNGEE=0)
    endif()
else()
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_BUNGEE=0)
endif()

set(MLRVST_ESSENTIA_ENABLED FALSE)
find_path(MLRVST_ESSENTIA_INCLUDE_DIR
    NAMES essentia/essentia.h
    PATHS
        "${MLRVST_ESSENTIA_PREFIX}/include"
        /opt/homebrew/include
        /usr/local/include)
find_library(MLRVST_ESSENTIA_LIBRARY
    NAMES essentia libessentia
    PATHS
        "${MLRVST_ESSENTIA_PREFIX}/lib"
        /opt/homebrew/lib
        /usr/local/lib)

if(MLRVST_ESSENTIA_INCLUDE_DIR AND MLRVST_ESSENTIA_LIBRARY)
    target_include_directories(mlrVST SYSTEM PRIVATE
        "${MLRVST_ESSENTIA_INCLUDE_DIR}"
        /opt/homebrew/Cellar/eigen/5.0.1/include/eigen3)
    target_link_libraries(mlrVST PRIVATE "${MLRVST_ESSENTIA_LIBRARY}")
    if(APPLE)
        find_library(ACCELERATE_FRAMEWORK Accelerate)
        if(ACCELERATE_FRAMEWORK)
            target_link_libraries(mlrVST PRIVATE "${ACCELERATE_FRAMEWORK}")
        endif()
    endif()
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_ESSENTIA_NATIVE=1)
    set(MLRVST_ESSENTIA_ENABLED TRUE)
else()
    message(WARNING "Native Essentia not found; building with internal Flip analysis only.")
    target_compile_definitions(mlrVST PRIVATE MLRVST_ENABLE_ESSENTIA_NATIVE=0)
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
        "${MLRVST_GENERATED_DIR}"
)

add_dependencies(mlrVST mlrVST_build_info)

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
message(STATUS "  SoundTouch swing path: ${MLRVST_SOUNDTOUCH_ENABLED}")
message(STATUS "  Bungee backend: ${MLRVST_BUNGEE_ENABLED}")
message(STATUS "")
message(STATUS "Plugin Formats:")
message(STATUS "  ✓ VST3")
if(APPLE)
    message(STATUS "  ✓ AU (Audio Unit)")
endif()
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
