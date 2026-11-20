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
// Caves live in the image's int3 alignment sea; their expected bytes are the sea's 0xCCs,
// so toggling off hands the padding back exactly. The runtime swaps at most 64 bytes per
// edit, so longer caves are split into consecutive edits.
// Infinite health hooks the damage routine (0x9f91df) where it computes the victim's new health
// (current [rdi+0xd0] minus damage) before the virtual SetHealth call (vtable slot 0x198). The
// player and enemies share the health class and SetHealth. The object at [rdi+0x100] is the
// first health listener: every AI-driven character carries the AI listener (first field
// 0xa3dd20), while the player carries a HUD listener that changes with the chapter (0x4c0f10 in
// one save, 0x578650 in another). So a victim is protected when its listener is not the AI one
// and its maximum health ([rdi+0xd4]) is at least 250.0f: the player has 450, the logged enemies
// 100-125. Logged in play with a diagnostic build on CUSA00785 1.02 in two chapters.
// v3 dropped the subtraction for every victim (enemies stopped dying), v4 hooked ApplyHealth's
// clamp, v5 compared the SetHealth slot (shared), v6 required the 0x4c0f10 HUD listener (other
// chapters use another one). The address is RIP-relative, so it holds wherever the image is
// mapped; r10/r11 are scratch here (r11 was stored just before, r10 is reloaded after the call).
// Cave at 0xef9d30 (65 bytes of an 85-byte int3 run).
constexpr unsigned char kDamageCompute[] = {0xc5, 0xfa, 0x10, 0x8f, 0xd0, 0x00, 0x00, 0x00,  // vmovss xmm1, [rdi+0xd0]
                                            0xc5, 0xf2, 0x5c, 0xc0};                         // vsubss xmm0, xmm1, xmm0
constexpr unsigned char kHealthJmp[] = {
    0xe9, 0xe1, 0x0a, 0x50, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // jmp 0xef9d30
};
constexpr unsigned char kHealthCave[] = {
    // vmovss xmm1,[rdi+0xd0]; mov r11,[rdi+0x100]; test r11,r11; jz 1f; mov r11,[r11];
    // lea r10,[rip->0xa3dd20]; cmp r11,r10; je 1f; cmp dword [rdi+0xd4],250.0f; jl 1f;
    // vmovaps xmm0,xmm1; jmp 0x9f9256; 1: vsubss xmm0,xmm1,xmm0; jmp 0x9f9256
    0xc5, 0xfa, 0x10, 0x8f, 0xd0, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x9f, 0x00,
    0x01, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x24, 0x4d, 0x8b, 0x1b, 0x4c,
    0x8d, 0x15, 0xd2, 0x3f, 0xb4, 0xff, 0x4d, 0x39, 0xd3, 0x74, 0x15, 0x81,
    0xbf, 0xd4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7a, 0x43, 0x7c, 0x09, 0xc5,
    0xf8, 0x28, 0xc1, 0xe9, 0xee, 0xf4, 0xaf, 0xff, 0xc5, 0xf2, 0x5c, 0xc0,
    0xe9, 0xe5, 0xf4, 0xaf, 0xff,
};
constexpr unsigned char kSea65[] = {
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};
// Throwables: the pool at [entity+0x228]+0x160 keeps the count at +0x310; using an item spends
// its per-use cost ([item+0xc4]) there (0x3d07e6, 0x491ce1, the latter after an "enough?" check).
constexpr unsigned char kSpendPoolR14[] = {0x41, 0x29, 0x86, 0x10, 0x03, 0x00, 0x00};  // sub [r14+0x310], eax
constexpr unsigned char kSpendPoolR15[] = {0x41, 0x29, 0x87, 0x10, 0x03, 0x00, 0x00};  // sub [r15+0x310], eax
constexpr unsigned char kNop7[] = {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};
constexpr unsigned char kSubtractRounds[] = {0x29, 0xd8};
constexpr unsigned char kNop2[] = {0x90, 0x90};
constexpr unsigned char kChargeMagazine[] = {0x29, 0x82, 0x10, 0x03, 0x00, 0x00};
constexpr unsigned char kNop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kDecrementCount[] = {0xff, 0x8f, 0x14, 0x03, 0x00, 0x00};  // dec dword [rdi+0x314]
// The second throw path computes reserve - 1 in eax at 0x6e7c0b and stores it to [r15+0x314];
// it follows the same event call (0x76e950) as the first. The unconditional jmp at 0x6e7c09
// skips it only on the fall-through path: 0x6e7c0b is a branch target. Found in play: the HUD
// reserve (right-hand number) still fell with only 0x6ed30d patched, and stopped falling
// once both were patched.
constexpr unsigned char kDecEax[] = {0xff, 0xc8};  // dec eax
constexpr unsigned char kNop2b[] = {0x66, 0x90};   // xchg ax,ax
constexpr unsigned char kHealthStore[] = {0xc4, 0xc1, 0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00};
// One-hit kill hooks ApplyHealth's store of the new health (0xa1059b). A victim that is not the
// player (same test as infinite health: health listener at [r12+0x100] other than the AI one and
// maximum health of at least 250.0f) and whose new health is below the current one (real damage,
// not healing or spawn values) gets 0 stored instead. The earlier cave zeroed xmm1, stored it and
// then stored the original xmm0 over it, so it never killed anything; it also told the player by
// the published tables' 0xF520 marker word, which this build does not carry. xmm0 stays the
// stored value (the caller divides it by max in xmm1 next); r10/r11 are dead until the call at
// 0xa105be. Cave at 0xf01030 (73 bytes), entered by a 10-byte jump.
constexpr unsigned char kOneHitJmp[] = {0xe9, 0x90, 0x0a, 0x4f, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90};  // jmp 0xf01030
constexpr unsigned char kOneHitCave[] = {
    // mov r11,[r12+0x100]; test r11,r11; jz 2f; mov r11,[r11]; lea r10,[rip->0xa3dd20];
    // cmp r11,r10; je 2f; cmp dword [r12+0xd4],250.0f; jge 3f;
    // 2: vucomiss xmm0,[r12+0xd0]; jae 3f; vxorps xmm0,xmm0,xmm0;
    // 3: vmovss [r12+0xd0],xmm0; jmp 0xa105a5
    0x4d, 0x8b, 0x9c, 0x24, 0x00, 0x01, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74,
    0x1d, 0x4d, 0x8b, 0x1b, 0x4c, 0x8d, 0x15, 0xd9, 0xcc, 0xb3, 0xff, 0x4d,
    0x39, 0xd3, 0x74, 0x0e, 0x41, 0x81, 0xbc, 0x24, 0xd4, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7a, 0x43, 0x7d, 0x10, 0xc4, 0xc1, 0x78, 0x2e, 0x84, 0x24,
    0xd0, 0x00, 0x00, 0x00, 0x73, 0x04, 0xc5, 0xf8, 0x57, 0xc0, 0xc4, 0xc1,
    0x7a, 0x11, 0x84, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xe9, 0x2c, 0xf5, 0xb0,
    0xff,
};
constexpr unsigned char kSea73[] = {
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};

static_assert(sizeof(kHealthCave) == sizeof(kSea65), "health cave must fill its sea span exactly");
// The runtime swaps at most 64 bytes per edit, so the cave goes in two pieces; caves come before
// the jump that enters them (SetEnabled writes in table order and restores in reverse).
constexpr Edit kHealthEdits[] = {{0xef9d30, kSea65, kHealthCave, 32},
                                 {0xef9d50, kSea65 + 32, kHealthCave + 32, sizeof(kSea65) - 32},
                                 {0x9f924a, kDamageCompute, kHealthJmp, sizeof(kDamageCompute)}};
constexpr Edit kAmmoEdits[] = {{0x752542, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x55520f, kSubtractRounds, kNop2, sizeof(kSubtractRounds)},
                               {0x70c038, kChargeMagazine, kNop6, sizeof(kChargeMagazine)}};
constexpr Edit kGrenadeEdits[] = {{0x6ed30d, kDecrementCount, kNop6, sizeof(kDecrementCount)},
                                  {0x6e7c0b, kDecEax, kNop2b, sizeof(kDecEax)},
                                  {0x3d07e6, kSpendPoolR14, kNop7, sizeof(kSpendPoolR14)},
                                  {0x491ce1, kSpendPoolR15, kNop7, sizeof(kSpendPoolR15)}};
static_assert(sizeof(kOneHitCave) == sizeof(kSea73), "one-hit cave must fill its sea span exactly");
constexpr Edit kOneHitEdits[] = {{0xf01030, kSea73, kOneHitCave, 36},
                                 {0xf01054, kSea73 + 36, kOneHitCave + 36, sizeof(kSea73) - 36},
                                 {0xa1059b, kHealthStore, kOneHitJmp, sizeof(kHealthStore)}};

// QTE button-press fix (issue #43, ported from the saxons-kicks/theorder1886-port patch
// "QTE Fix - Button Mash at Any Frame Rate"): in sub_b105b0 each mash press is scaled by the
// frame time (dt x 30) before it reaches the meter, so at 60 FPS a press counts 0.5 and at
// 120 FPS a quarter while the meter still drains per second — the prologue drowning QTE
// (their #101; the same math behind our #33 "need turbo") could not be finished above 30 FPS.
// jbe -> jmp skips the scaling unconditionally; at 30 FPS the branch is taken already, so the
// fix changes nothing there. Site verified in the CUSA00785 APP_VER 01.02 image through the
// production decode path: 0x76 0x08 0xc5 at RVA 0xb106e7.
constexpr unsigned char kQtePressBranch[] = {0x76, 0x08, 0xc5};  // jbe +8; vmulss xmm0,...
constexpr unsigned char kQtePressSkip[] = {0xeb, 0x08, 0xc5};    // jmp +8: the press counts 1.0
constexpr Edit kQteFixEdits[] = {{0xb106e7, kQtePressBranch, kQtePressSkip, sizeof(kQtePressBranch)}};

constexpr Cheat kTable[] = {
    {"unlimited-health", "Infinite health",
     "Damage to the player is ignored: health never falls, while healing still works. "
     "Enemies take damage as usual. Turn it off for sequences that wait for the player "
     "to fall.",
     kHealthEdits, std::size(kHealthEdits)},
    {"unlimited-ammo", "Infinite ammunition",
     "Firing never subtracts rounds and reloading never charges them against the reserve: "
     "the magazine and the reserve both stay put.",
     kAmmoEdits, std::size(kAmmoEdits)},
    {"unlimited-grenades", "Infinite throwables",
     "Throwing does not spend from the throwable count.",
     kGrenadeEdits, std::size(kGrenadeEdits)},
    {"one-hit-kill", "Enemy one-hit kill",
     "Any real damage to a character that is not the player sets its health to zero: one hit "
     "kills. Healing and spawn values pass through untouched, so nothing dies on creation.",
     kOneHitEdits, std::size(kOneHitEdits)},
    {"qte-button-fix", "QTE button-press fix",
     "Button-mash quick-time events count every press in full at any frame rate. Without it the "
     "game shrinks each press by the frame time above 30 FPS, so bars like the prologue drowning "
     "sequence fall faster than you can press. On by default when the game starts above 30 FPS; "
     "switching it here is remembered and wins over the default. It does not touch the Chapters "
     "6 and 9 stuck sequences, which are a different problem.",
     kQteFixEdits, std::size(kQteFixEdits)},
};

bool Matches(const unsigned char* found, const unsigned char* bytes, size_t length) {
    return std::memcmp(found, bytes, length) == 0;
}

}  // namespace

const Cheat* Table() { return kTable; }
size_t Count() { return sizeof(kTable) / sizeof(kTable[0]); }

unsigned QteFixBit() {
    for (size_t i = 0; i < Count(); ++i) {
        if (std::strcmp(Table()[i].id, "qte-button-fix") == 0) {
            return 1u << i;
        }
    }
    return 0;
}

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
    // Enabling writes in table order (caves before the jumps into them); disabling restores in
    // reverse, so a running thread never enters a half-written or already-restored cave.
    for (size_t n = 0; n < cheat.edit_count; ++n) {
        const Edit& edit = cheat.edits[on ? n : cheat.edit_count - 1 - n];
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
