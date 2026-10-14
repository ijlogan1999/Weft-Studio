// TestCrashScan: crashes as soon as a host instantiates it, so a plugin scanner
// must survive it (AT-PLG-3). Only the out-of-process scanner should ever load it.
// A null write rather than abort() avoids the MSVC debug-runtime abort dialog.

#include "FixtureEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    volatile int* nowhere = nullptr;
    *nowhere = 1;
    return nullptr;
}
