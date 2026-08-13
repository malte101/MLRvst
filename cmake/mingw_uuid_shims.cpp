// Compiled only into the unsupported MinGW/Windows cross-build.
//
// JUCE's juce_ComSmartPtr_windows.h installs its own __uuidof (a UUIDGetter
// template) only when the compiler has not already defined one. mingw always
// defines __uuidof — as __mingw_uuidof<__typeof(x)>() — so JUCE keeps mingw's
// version. That breaks on JUCE's Direct2D code, which calls __uuidof on a
// ComSmartPtr *value* rather than on the interface type:
//
//     ComSmartPtr<IDXGISurface> surface;
//     chain->GetBuffer (0, __uuidof (surface), ...);
//
// mingw therefore asks for the UUID of the wrapper type, which nothing declares,
// and the build dies at link time with an undefined __mingw_uuidof<ComSmartPtr<T>>.
// Define those specialisations here and forward each to the interface it wraps,
// which is the IID the D3D/DXGI calls actually expect.

#include <windows.h>
#include <dxgi.h>

namespace juce
{
    template <class ComClass> class ComSmartPtr;
}

template <>
const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGISurface>>()
{
    return __mingw_uuidof<IDXGISurface>();
}

template <>
const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGIDevice>>()
{
    return __mingw_uuidof<IDXGIDevice>();
}
