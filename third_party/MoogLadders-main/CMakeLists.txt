cmake_minimum_required(VERSION 3.10)
project(MoogLadders VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Main example with audio playback
add_executable(MoogLadderExample
    example/main.cpp
    src/AudioDevice.cpp
    third_party/rtaudio/RtAudio.cpp
)

target_include_directories(MoogLadderExample PRIVATE
    src
    third_party
    example
)

# Run all filters tool (no audio device dependency)
add_executable(RunFilters
    example/run-filters.cpp
)

target_include_directories(RunFilters PRIVATE
    src
    example
)

# Option to use RtAudio dummy backend (for CI environments without audio hardware)
option(RTAUDIO_DUMMY "Use RtAudio dummy backend (no audio output)" OFF)

# Platform-specific RtAudio configuration for MoogLadderExample
if(RTAUDIO_DUMMY)
    # Dummy backend - no platform API defined, RtAudio auto-selects dummy
    message(STATUS "Using RtAudio dummy backend")
elseif(WIN32)
    target_compile_definitions(MoogLadderExample PRIVATE __WINDOWS_DS__)
    target_link_libraries(MoogLadderExample PRIVATE dsound ole32 winmm)
elseif(APPLE)
    target_compile_definitions(MoogLadderExample PRIVATE __MACOSX_CORE__)
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    target_link_libraries(MoogLadderExample PRIVATE ${COREAUDIO_LIB} ${COREFOUNDATION_LIB})
else()
    # Linux - use ALSA
    target_compile_definitions(MoogLadderExample PRIVATE __LINUX_ALSA__)
    find_package(ALSA REQUIRED)
    target_link_libraries(MoogLadderExample PRIVATE ALSA::ALSA pthread)
endif()
