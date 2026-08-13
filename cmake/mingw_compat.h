// Force-included (via -include) into every C++ translation unit of the
// unsupported MinGW/Windows build. It exists so we never have to patch the
// vendored JUCE tree — JUCE is not checked in here, so a local edit would be
// silently lost on the next JUCE refresh.
//
// Everything below is a gap between what JUCE assumes about a Windows compiler
// and what the mingw-w64 headers actually provide.

#pragma once

// 0) JUCE's Windows sources reach for mem*/str* without always including it.
//    (This used to be a separate `-include cstring`, but CMake de-duplicates
//    repeated `-include` flags and orphans the second path, so it lives here.)
#include <cstring>

// 1) JUCE only pulls <intrin.h> in for MSVC, but juce_SystemStats_windows.cpp
//    calls the MSVC-style __cpuid(int[4], int) on every non-clang compiler.
//    mingw-w64 ships that intrinsic — it just needs the header.
#if defined(__x86_64__) || defined(__i386__)
 #include <intrin.h>
#endif

// 2) juce_JPEGLoader.cpp does `using jpeglibNamespace::boolean;` on Windows for
//    every compiler except MSVC/clang. That name only exists if jpglib's
//    jconfig.h emitted its own typedef, which it skips once rpcndr.h has been
//    read — and on mingw the COM/Direct2D header chain always reads rpcndr.h
//    first. Declare the typedef in the namespace ourselves; it is identical to
//    both the jconfig.h and rpcndr.h spelling, so a later re-typedef is a legal
//    no-op redeclaration rather than a conflict.
namespace juce
{
    namespace jpeglibNamespace
    {
        typedef unsigned char boolean;
    }
}
