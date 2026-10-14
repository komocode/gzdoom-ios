// Portable Apple builds use the bundled OPL and TiMidity synthesizers.
// FluidSynth's desktop GLib dependency is optional.
#include <stdexcept>
#include "zmusic/zmusic_internal.h"
#include "mididevice.h"
FluidConfig fluidConfig;
MIDIDevice *CreateFluidSynthMIDIDevice(int samplerate, const char *args)
{
    throw std::runtime_error("FluidSynth is unavailable in this build; select OPL or TiMidity.");
}
