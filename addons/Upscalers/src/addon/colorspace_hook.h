// ColorSpaceHook: the colour space each swap chain in the process presents in. DXGI can set it (IDXGISwapChain3::SetColorSpace1) but not
// tell it, and a 10-bit back buffer may hold HDR10 (PQ) or plain SDR whatever state the display is in, so it is followed as it is set: the
// swap chains made (each starts in its format's default: scRGB for half floats, sRGB otherwise) and each SetColorSpace1, by patching their
// slots in DXGI's shared function tables, as PresentHook does for Present. Put in place at the addon's start, before Lossless Scaling makes
// the swap chain it scales into; a swap chain made before that is simply not known (hdr.h then decides from the display, as before).
#pragma once
#include <dxgi1_4.h>
#include <functional>

namespace ColorSpaceHook {
    using LogFn = std::function<void(const char*)>;
    bool Install(LogFn log);   // with a short-lived device and swap chain of its own; true when already in place
    bool Installed();
    // The colour space of this swap chain (any of its interfaces), if it was made or set after Install.
    bool Known(IUnknown* chain, DXGI_COLOR_SPACE_TYPE& space);
}
