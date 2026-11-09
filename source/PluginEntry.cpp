#include "Xenon.h"

/**
    The one registration.

    This file is listed directly in the Xenon MODULE target, not in
    xenon_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Xenon`, eight characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class XenonEffect : public xenon::XenonPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< XenonEffect >,// Create method
	"XN01",                      // Plugin unique ID of maximum length 4
	"SW Xenon",                  // Plugin name
	2,                           // API major version number
	1,                           // API minor version number
	0,                           // Plugin major version number
	1,                           // Plugin minor version number
	FF_EFFECT,                   // Plugin type
	"Every slice of your Advanced Output a strobe head, fired on a desk-style chase.\n\nXenon reads the slices from "
	"Arena's active Advanced Output preset (or a preset you pick, or a plain grid) and gives each one a xenon strobe: a "
	"capacitor bank that recharges through a resistor and a tube that dumps it in a flash. The chase steps on the beat, "
	"an onset or a press of Fire, so a fast chase on a slow recharge fires weaker and weaker flashes, as a real rig does.",
	"Xenon FFGL effect"// About
);

extern "C" const char* XenonBuildStamp()
{
	return "xenon " XENON_VERSION ", built " __DATE__ " " __TIME__;
}
