#pragma once

// The window the host gives the editor. Some hosts (Bitwig on Linux when its window is maximised)
// grow their window without telling the plug-in: the editor watches it and follows.

#include <juce_gui_basics/juce_gui_basics.h>

namespace concha::ui {

struct HostWindowSizes {
    int ownW = 0, ownH = 0;       // the editor's native window, in pixels
    int parentW = 0, parentH = 0; // the host's window that holds it, in pixels
};
// false when unknown (not Linux / X11, no parent window)
bool hostWindowSizes(juce::ComponentPeer* peer, HostWindowSizes& out);

} // namespace concha::ui
