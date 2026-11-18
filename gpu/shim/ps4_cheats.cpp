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
// The one-hit-kill cave lives in the image's int3 alignment sea at 0xf0102d (60 of its
// 126 bytes); its expected bytes are the sea's 0xCCs, so toggling off hands the padding
// back exactly. Health needs no cave: the player's damage routine subtracts damage from
// current health in four bytes, and a same-size copy replaces the subtraction.
constexpr unsigned char kHealthSubtract[] = {0xc5, 0xf2, 0x5c, 0xc0};        // vsubss xmm0, xmm1, xmm0
constexpr unsigned char kHealthKeep[] = {0xf3, 0x0f, 0x10, 0xc1};            // movss xmm0, xmm1
constexpr unsigned char kSubtractRounds[] = {0x29, 0xd8};
constexpr unsigned char kNop2[] = {0x90, 0x90};
constexpr unsigned char kChargeMagazine[] = {0x29, 0x82, 0x10, 0x03, 0x00, 0x00};
constexpr unsigned char kNop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kDecrementCount[] = {0xff, 0x8f, 0x14, 0x03, 0x00, 0x00};  // dec dword [rdi+0x314]
constexpr unsigned char kHealthStore[] = {0xc4, 0xc1, 0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00};
constexpr unsigned char kOneHitJmp[] = {0xe9, 0x8d, 0x0a, 0x4f, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kOneHitCave[] = {
    0x66, 0x41, 0x81, 0x7c, 0x24, 0x30, 0x20, 0xf5, 0x74, 0x19, 0xc4, 0xc1,
    0xf8, 0x2e, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x73, 0x0d, 0x0f, 0x57,
    0xc9, 0xc4, 0xc1, 0x7a, 0x11, 0x8c, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xc4,
    0xc1, 0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xc4, 0xc1, 0xfa,
    0x10, 0x8c, 0x24, 0xd4, 0x00, 0x00, 0x00, 0xe9, 0x3c, 0xf5, 0xb0, 0xff,
};
constexpr unsigned char kSea60[] = {
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};

constexpr Edit kHealthEdits[] = {{0x9f9252, kHealthSubtract, kHealthKeep, sizeof(kHealthSubtract)}};
constexpr Edit kAmmoEdits[] = {{0x752542, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x55520f, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x70c038, kChargeMagazine, kNop6, sizeof(kChargeMagazine)}};
constexpr Edit kGrenadeEdits[] = {{0x6ed30d, kDecrementCount, kNop6, sizeof(kDecrementCount)}};
constexpr Edit kOneHitEdits[] = {{0xa1059b, kHealthStore, kOneHitJmp, sizeof(kHealthStore)},
                                 {0xf0102d, kSea60, kOneHitCave, sizeof(kSea60)}};

constexpr Cheat kTable[] = {
    {"unlimited-health", "Infinite health",
     "The player's damage routine no longer subtracts the incoming damage from current "
     "health: hits register and animate, but health never falls. Enemies are unaffected. "
     "Turn it off for sequences that wait for the player to fall.",
     kHealthEdits, std::size(kHealthEdits)},
    {"unlimited-ammo", "Infinite ammunition",
     "Firing never subtracts rounds and reloading never charges them against the reserve: "
     "the magazine and the reserve both stay put.",
     kAmmoEdits, std::size(kAmmoEdits)},
    {"unlimited-grenades", "Infinite throwables",
     "The inventory's throwable count is never decremented when one is thrown.",
     kGrenadeEdits, std::size(kGrenadeEdits)},
    {"one-hit-kill", "Enemy one-hit kill",
     "Any real damage to an entity that is not the player sets its health to zero: one hit "
     "kills. Healing and spawn values pass through untouched, so nothing dies on creation.",
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
