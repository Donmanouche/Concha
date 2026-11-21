#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include "plugin/ui/HostWindow.h"

namespace concha::ui {

bool hostWindowSizes(juce::ComponentPeer* peer, HostWindowSizes& out)
{
#if JUCE_LINUX || JUCE_BSD
    if (peer == nullptr) return false;
    auto* display = juce::XWindowSystem::getInstance()->getDisplay();
    auto* x = juce::X11Symbols::getInstance();
    if (display == nullptr || x == nullptr) return false;
    const auto window = (::Window)peer->getNativeHandle();
    if (window == 0) return false;
    juce::XWindowSystemUtilities::ScopedXLock lock;
    ::Window root = 0, parent = 0, *children = nullptr;
    unsigned int count = 0;
    if (!x->xQueryTree(display, window, &root, &parent, &children, &count)) return false;
    if (children) x->xFree(children);
    if (parent == 0 || parent == root) return false; // a top-level window: no host window around it
    ::Window r;
    int px, py;
    unsigned int w, h, border, depth;
    if (!x->xGetGeometry(display, window, &r, &px, &py, &w, &h, &border, &depth)) return false;
    out.ownW = (int)w, out.ownH = (int)h;
    if (!x->xGetGeometry(display, parent, &r, &px, &py, &w, &h, &border, &depth)) return false;
    out.parentW = (int)w, out.parentH = (int)h;
    return true;
#else
    juce::ignoreUnused(peer, out);
    return false;
#endif
}

} // namespace concha::ui
