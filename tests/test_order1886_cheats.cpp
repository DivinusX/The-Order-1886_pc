// Cheats (issue #24): the Mods section's guarded instruction swap, tested against a
// synthetic image. The loader's runtime_guest_peek / runtime_guest_patch are re-implemented
// here over a flat buffer so the state machine can run without a game executable; the
// byte-for-byte check against the real APP_VER 01.02 image is the verification note's job
// (docs/verification), not this test's.
#include "ps4_cheats.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
constexpr uint64_t kImageSize = 0xC00000;  // spans the real table's highest site (0xb106e7)
unsigned char g_image[kImageSize];

void Put(uint64_t rva, const unsigned char* bytes, size_t length) {
    std::memcpy(g_image + rva, bytes, length);
}

bool At(uint64_t rva, const unsigned char* bytes, size_t length) {
    return std::memcmp(g_image + rva, bytes, length) == 0;
}

const unsigned char kVerified[] = {0x29, 0xd8};            // sub eax, ebx
const unsigned char kCheated[] = {0x90, 0x90};             // nop; nop
const unsigned char kOther[] = {0x0f, 0xb6};               // any other build
const unsigned char kFiller[] = {0xCC, 0xCC};              // the "no image content" filler
constexpr Ps4Cheats::Edit kEdits[] = {{0x1000, kVerified, kCheated, sizeof(kVerified)},
                                      {0x2000, kVerified, kCheated, sizeof(kVerified)}};
const Ps4Cheats::Cheat kTestCheat = {"test", "Test", "Body", kEdits, 2};
}  // namespace

// The loader's guarded image access, re-implemented over the synthetic buffer. A real
// refusal (-3) must mean the bytes differ, exactly like the loader reports it.
extern "C" int runtime_guest_peek(uint64_t rva, unsigned char* out, size_t length) {
    if (!out || !length || length > 64 || rva + length > kImageSize) {
        return -2;
    }
    std::memcpy(out, g_image + rva, length);
    return 0;
}

extern "C" int runtime_guest_patch(uint64_t rva, const unsigned char* expected,
                                   const unsigned char* replacement, size_t length) {
    if (!expected || !replacement || !length || length > 64 || rva + length > kImageSize) {
        return -2;
    }
    if (std::memcmp(g_image + rva, expected, length) != 0) {
        return -3;
    }
    std::memcpy(g_image + rva, replacement, length);
    return 0;
}

int main() {
    // The table is well-formed and every id is unique.
    const Ps4Cheats::Cheat* table = Ps4Cheats::Table();
    for (size_t i = 0; i < Ps4Cheats::Count(); ++i) {
        assert(table[i].id && *table[i].id);
        assert(table[i].title && *table[i].title);
        assert(table[i].body && *table[i].body);
        assert(table[i].edit_count > 0);
        for (size_t e = 0; e < table[i].edit_count; ++e) {
            const Ps4Cheats::Edit& edit = table[i].edits[e];
            assert(edit.length > 0 && edit.length <= 64);
            assert(edit.expected && edit.replacement);
        }
        for (size_t j = i + 1; j < Ps4Cheats::Count(); ++j) {
            assert(std::strcmp(table[i].id, table[j].id) != 0);
        }
    }

    // With no image loaded, nothing is available and nothing changes.
    std::memset(g_image, 0xCC, sizeof(g_image));
    assert(Ps4Cheats::Of(kTestCheat) == Ps4Cheats::State::Unknown);
    std::string problem;
    assert(!Ps4Cheats::SetEnabled(kTestCheat, true, problem));
    assert(!problem.empty());
    assert(At(0x1000, kFiller, 2) && At(0x2000, kFiller, 2));

    // The verified bytes read as Off; applying turns both sites; reverting restores them.
    Put(0x1000, kVerified, 2);
    Put(0x2000, kVerified, 2);
    assert(Ps4Cheats::Of(kTestCheat) == Ps4Cheats::State::Off);
    assert(Ps4Cheats::SetEnabled(kTestCheat, true, problem));
    assert(At(0x1000, kCheated, 2) && At(0x2000, kCheated, 2));
    assert(Ps4Cheats::Of(kTestCheat) == Ps4Cheats::State::On);
    assert(Ps4Cheats::SetEnabled(kTestCheat, false, problem));
    assert(At(0x1000, kVerified, 2) && At(0x2000, kVerified, 2));

    // A second edit that does not match refuses the whole cheat, and the site that did
    // match keeps its verified bytes: no half-patched image.
    Put(0x1000, kVerified, 2);
    Put(0x2000, kOther, 2);
    assert(Ps4Cheats::Of(kTestCheat) == Ps4Cheats::State::Unknown);
    problem.clear();
    assert(!Ps4Cheats::SetEnabled(kTestCheat, true, problem));
    assert(!problem.empty());
    assert(At(0x1000, kVerified, 2) && At(0x2000, kOther, 2));

    // graphics.ini's id list round-trips through the mask, ignoring unknown ids.
    assert(Ps4Cheats::QteFixBit() == (1u << (Ps4Cheats::Count() - 1)));  // table order is bits
    const unsigned mask = Ps4Cheats::MaskFromIds("unlimited-health, one-hit-kill, unknown-id");
    assert(mask == (1u | (Ps4Cheats::QteFixBit() >> 1)));
    const std::string ids = Ps4Cheats::IdsFromMask(mask);
    assert(Ps4Cheats::MaskFromIds(ids) == mask);
    assert(Ps4Cheats::IdsFromMask(0).empty());
    assert(Ps4Cheats::MaskFromIds("") == 0 && Ps4Cheats::MaskFromIds(",, ,") == 0);
    assert(Ps4Cheats::IdsFromMask(Ps4Cheats::QteFixBit()) == "qte-button-fix");
    assert(Ps4Cheats::MaskFromIds("qte-button-fix") == Ps4Cheats::QteFixBit());

    // The QTE button-press fix (issue #43) is the last row with two sites: the branch
    // swap (jbe 0x76 -> jmp 0xeb at 0xb106e7) AND the NOP of the dt multiply at
    // 0xb10772 — only together does a press count 1.0 at any frame rate (the
    // contributor's single-site patch made presses 30x weaker; owner play test).
    const Ps4Cheats::Cheat& qte = table[Ps4Cheats::Count() - 1];
    assert(std::strcmp(qte.id, "qte-button-fix") == 0);
    assert(qte.edit_count == 2);
    assert(qte.edits[0].rva == 0xb106e7 && qte.edits[0].length == 3);
    assert(qte.edits[0].expected[0] == 0x76 && qte.edits[0].replacement[0] == 0xeb);
    assert(qte.edits[1].rva == 0xb10772 && qte.edits[1].length == 9);
    assert(qte.edits[1].expected[3] == 0x59 && qte.edits[1].replacement[0] == 0x90);
    std::memset(g_image, 0xCC, sizeof(g_image));
    Put(0xb106e7, qte.edits[0].expected, 3);
    Put(0xb10772, qte.edits[1].expected, 9);
    assert(Ps4Cheats::Of(qte) == Ps4Cheats::State::Off);
    assert(Ps4Cheats::SetEnabled(qte, true, problem));
    assert(Ps4Cheats::Of(qte) == Ps4Cheats::State::On);
    assert(At(0xb106e7, qte.edits[0].replacement, 3) && At(0xb10772, qte.edits[1].replacement, 9));
    assert(Ps4Cheats::SetEnabled(qte, false, problem));
    assert(At(0xb106e7, qte.edits[0].expected, 3) && At(0xb10772, qte.edits[1].expected, 9));
    Put(0xb106e7, qte.edits[0].expected, 2);  // a build that differs at the guard byte
    g_image[0xb106e7 + 2] = 0x0f;             // (e.g. another instruction there) is refused
    assert(Ps4Cheats::Of(qte) == Ps4Cheats::State::Unknown);
    problem.clear();
    assert(!Ps4Cheats::SetEnabled(qte, true, problem) && !problem.empty());
    assert(At(0xb10772, qte.edits[1].expected, 9));  // refusal wrote nothing, not half

    std::printf("Cheats tests passed.\n");
    return 0;
}
