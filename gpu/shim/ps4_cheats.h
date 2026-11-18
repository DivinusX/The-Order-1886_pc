// SPDX-License-Identifier: GPL-2.0-or-later
// Cheats for the in-game menu's Mods section (issue #24): instruction-site patches applied
// to the loaded image while the game runs. Every edit names the bytes it expects to find,
// so a build whose instructions differ is refused instead of patched blind — the same rule
// the launcher's preparation recipes follow (scripts/order1886_patches.py).
// The sites come from the published PS4 cheat tables in the GoldHEN cheat repository
// (health and one-hit kill after Wander_9.00's and Fullcodes' hooks; the ammunition and
// grenade sites by Fullcodes), re-expressed against the image's own int3 alignment sea
// instead of the tables' scratch addresses. Health clamps to the higher of current and
// incoming rather than storing the tables' 600.0f: the overwrite corrupted spawn and
// initialization values on this runtime and crashed the title within seconds. The player
// is told apart from other entities by the marker word the published tables test.
// Byte-for-byte verified against the CUSA00785 APP_VER 01.02 image; gameplay acceptance
// remains open per cheat.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace Ps4Cheats {

struct Edit {
    uint64_t rva;             ///< image-relative address, as the preparation recipes count it
    const unsigned char* expected;   ///< the verified build's instruction bytes
    const unsigned char* replacement; ///< the same-size cheat encoding
    size_t length;
};

struct Cheat {
    const char* id;     ///< stable key: written to graphics.ini, never shown
    const char* title;  ///< the menu row label
    const char* body;   ///< what the menu explains when the row is selected
    const Edit* edits;
    size_t edit_count;
};

/// The table, in menu order. A cheat's bit in Ps4Settings::cheat_mask is its index here.
const Cheat* Table();
size_t Count();

/// Bit mask of the table entries whose ids appear in a graphics.ini cheat list; unknown
/// ids are ignored so an older file never enables the wrong row.
unsigned MaskFromIds(const std::string& ids);
/// The table ids whose bits are set, comma-separated ("" for none).
std::string IdsFromMask(unsigned mask);

enum class State { Off, On, Unknown };

/// What the image currently holds at the cheat's sites: On or Off only when every site
/// matches the table byte for byte; Unknown means this executable is not the verified build.
State Of(const Cheat& cheat);

/// Applies the cheat (on) or restores the verified bytes (off). False with a reason when
/// any site refuses the swap; every site is checked before the first byte is written.
bool SetEnabled(const Cheat& cheat, bool on, std::string& problem);

/// Startup pass for graphics.ini's saved selection: every enabled cheat that the loaded
/// image accepts. Reports skipped ones on stdout, like the other boot-time settings.
void ApplySaved(unsigned mask);

}  // namespace Ps4Cheats
