// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_cheats.h"

#include <cstdio>
#include <cstring>

// The loader's guarded image access (src/probe.c): reads and instruction swaps that check
// the expected bytes and restore the page's own protection flags afterwards.
extern "C" int runtime_guest_peek(uint64_t rva, unsigned char* out, size_t length);
extern "C" int runtime_guest_patch(uint64_t rva, const unsigned char* expected,
                                   const unsigned char* replacement, size_t length);

namespace Ps4Cheats {
namespace {

// Instruction bytes from the CUSA00785 APP_VER 01.02 image, verified site by site.
// The two code caves live in the 126-byte int3 alignment sea at 0xf0102d..0xf010ab
// (executable, referenced by nothing): the health cave at 0xf0102d, the one-hit-kill
// cave at 0xf01060. A cave edit expects the sea's 0xCC bytes, so toggling off hands
// the padding back exactly as it was.
constexpr unsigned char kHealthStore[] = {0xc4, 0xc1, 0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00};
constexpr unsigned char kHealthJmp[] = {0xe9, 0x8d, 0x0a, 0x4f, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kHealthCave[] = {
    0x66, 0x41, 0x81, 0x7c, 0x24, 0x30, 0x20, 0xf5, 0x75, 0x0e, 0x41, 0xc7,
    0x84, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x44, 0xeb, 0x0a,
    0xc4, 0xc1, 0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xe9, 0x51,
    0xf5, 0xb0, 0xff,
};
constexpr unsigned char kSubtractRounds[] = {0x29, 0xd8};
constexpr unsigned char kNop2[] = {0x90, 0x90};
constexpr unsigned char kChargeMagazine[] = {0x29, 0x82, 0x10, 0x03, 0x00, 0x00};
constexpr unsigned char kNop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kDecrement[] = {0xff, 0xc8};
constexpr unsigned char kDamageStore[] = {0x4c, 0x89, 0x9f, 0xf0, 0x00, 0x00, 0x00};
constexpr unsigned char kDamageJmp[] = {0xe9, 0x1b, 0x7e, 0x50, 0x00, 0x90, 0x90};
constexpr unsigned char kOneHitCave[] = {
    0x4c, 0x89, 0x9f, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x07, 0xc5, 0xfa,
    0x10, 0x8f, 0xd0, 0x00, 0x00, 0x00, 0x66, 0x81, 0x7f, 0x30, 0x20, 0xf5,
    0x74, 0x06, 0xc5, 0xf8, 0x57, 0xc0, 0xeb, 0x04, 0xc5, 0xf2, 0x5c, 0xc0,
    0xe9, 0xcd, 0x81, 0xaf, 0xff,
};
// The alignment sea's bytes: what both caves expect to find before writing, and what
// toggling a cheat off hands back.
constexpr unsigned char kHealthSea[] = {
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};
constexpr unsigned char kOneHitSea[] = {
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};

constexpr Edit kHealthEdits[] = {{0xa1059b, kHealthStore, kHealthJmp, sizeof(kHealthStore)},
                                 {0xf0102d, kHealthSea, kHealthCave, sizeof(kHealthSea)}};
constexpr Edit kAmmoEdits[] = {{0x752542, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x55520f, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x70c038, kChargeMagazine, kNop6, sizeof(kChargeMagazine)}};
constexpr Edit kGrenadeEdits[] = {{0x6e7c0b, kDecrement, kNop2, sizeof(kDecrement)}};
constexpr Edit kOneHitEdits[] = {{0x9f9240, kDamageStore, kDamageJmp, sizeof(kDamageStore)},
                                 {0xf01060, kOneHitSea, kOneHitCave, sizeof(kOneHitSea)}};

constexpr Cheat kTable[] = {
    {"unlimited-health", "Infinite health",
     "Only the player keeps their health: the game's own player marker decides it, the "
     "same test the published cheat used. Hits register and animate, but the player's "
     "health never falls and enemies still die. Sequences that wait for the player to "
     "fall can behave differently while this is on; turn it off to progress.",
     kHealthEdits, std::size(kHealthEdits)},
    {"unlimited-ammo", "Infinite ammunition",
     "Firing never subtracts rounds and reloading never charges them against the reserve: "
     "the magazine and the reserve both stay put.",
     kAmmoEdits, std::size(kAmmoEdits)},
    {"unlimited-grenades", "Infinite throwables",
     "Grenades and other throwables are never counted down. Instruction site verified on "
     "APP_VER 01.02; gameplay acceptance is open.",
     kGrenadeEdits, std::size(kGrenadeEdits)},
    {"one-hit-kill", "Enemy one-hit kill",
     "Any damage to an entity that is not the player sets its health to zero: one hit "
     "kills. Friendly entities hit by the player die the same way; turn it off for "
     "sequences that need an actor to survive.",
     kOneHitEdits, std::size(kOneHitEdits)},
};

bool Matches(const unsigned char* found, const unsigned char* bytes, size_t length) {
    return std::memcmp(found, bytes, length) == 0;
}

}  // namespace

const Cheat* Table() { return kTable; }
size_t Count() { return sizeof(kTable) / sizeof(kTable[0]); }

State Of(const Cheat& cheat) {
    unsigned char found[64];
    bool all_on = true;
    bool all_off = true;
    for (size_t i = 0; i < cheat.edit_count; ++i) {
        const Edit& edit = cheat.edits[i];
        if (edit.length > sizeof(found) ||
            runtime_guest_peek(edit.rva, found, edit.length) != 0) {
            return State::Unknown;
        }
        all_on = all_on && Matches(found, edit.replacement, edit.length);
        all_off = all_off && Matches(found, edit.expected, edit.length);
    }
    if (all_on) {
        return State::On;
    }
    if (all_off) {
        return State::Off;
    }
    return State::Unknown;
}

bool SetEnabled(const Cheat& cheat, bool on, std::string& problem) {
    // Check every site before writing the first byte, so a refusal never half-patches.
    unsigned char found[64];
    for (size_t i = 0; i < cheat.edit_count; ++i) {
        const Edit& edit = cheat.edits[i];
        const unsigned char* pre = on ? edit.expected : edit.replacement;
        if (edit.length > sizeof(found)) {
            problem = "the cheat table declares an oversized edit";
            return false;
        }
        if (runtime_guest_peek(edit.rva, found, edit.length) != 0) {
            problem = "the game image is not loaded";
            return false;
        }
        if (!Matches(found, pre, edit.length)) {
            char site[24];
            std::snprintf(site, sizeof(site), "%#llx", static_cast<unsigned long long>(edit.rva));
            problem = std::string("the bytes at ") + site +
                      (on ? " are not the verified ones for this cheat"
                          : " are not the ones the cheat wrote, so the image changed elsewhere");
            return false;
        }
    }
    for (size_t i = 0; i < cheat.edit_count; ++i) {
        const Edit& edit = cheat.edits[i];
        const int result = runtime_guest_patch(edit.rva, on ? edit.expected : edit.replacement,
                                               on ? edit.replacement : edit.expected, edit.length);
        if (result != 0) {
            problem = result == -2 ? "a cheat site is outside the loaded image segments"
                                   : "the site changed between the check and the write";
            return false;
        }
    }
    return true;
}

unsigned MaskFromIds(const std::string& ids) {
    unsigned mask = 0;
    size_t position = 0;
    while (position < ids.size()) {
        const size_t comma = ids.find(',', position);
        std::string name = ids.substr(position, comma == std::string::npos ? std::string::npos
                                                                          : comma - position);
        position = comma == std::string::npos ? ids.size() : comma + 1;
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        for (size_t i = 0; i < Count(); ++i) {
            if (name == Table()[i].id) {
                mask |= 1u << i;
            }
        }
    }
    return mask;
}

std::string IdsFromMask(unsigned mask) {
    std::string ids;
    for (size_t i = 0; i < Count(); ++i) {
        if (mask & (1u << i)) {
            if (!ids.empty()) {
                ids += ',';
            }
            ids += Table()[i].id;
        }
    }
    return ids;
}

void ApplySaved(unsigned mask) {
    for (size_t i = 0; i < Count(); ++i) {
        if (!(mask & (1u << i))) {
            continue;
        }
        std::string problem;
        if (SetEnabled(Table()[i], true, problem)) {
            std::printf("Cheats: %s applied at start.\n", Table()[i].id);
        } else {
            std::printf("Cheats: saved cheat %s not applied: %s\n", Table()[i].id, problem.c_str());
        }
    }
    std::fflush(stdout);
}

}  // namespace Ps4Cheats
