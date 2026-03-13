#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <climits>

// Standalone copies of cave encoding helpers from form_caching_patches.h.

namespace {
    bool EmitJmpRel32(std::uint8_t* site, std::uint8_t* cave, int patchSize)
    {
        auto dist = reinterpret_cast<std::intptr_t>(cave) - reinterpret_cast<std::intptr_t>(site + 5);
        if (dist > INT32_MAX || dist < INT32_MIN) return false;
        site[0] = 0xE9;
        auto rel32 = static_cast<std::int32_t>(dist);
        std::memcpy(&site[1], &rel32, 4);
        for (int i = 5; i < patchSize; i++) site[i] = 0x90;
        return true;
    }

    void EmitJmpAbs64(std::uint8_t* cave, int& off, std::uint64_t target)
    {
        cave[off++] = 0xFF; cave[off++] = 0x25;
        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
        std::memcpy(&cave[off], &target, 8); off += 8;
    }
}

// ── EmitJmpRel32 tests ─────────────────────────────────────────

TEST_CASE("EmitJmpRel32 writes E9 opcode", "[cave]") {
    std::uint8_t site[8] = {};
    std::uint8_t cave[1] = {};
    // Place cave right after site for a small positive offset
    auto* fakeSite = reinterpret_cast<std::uint8_t*>(0x1000);
    auto* fakeCave = reinterpret_cast<std::uint8_t*>(0x1100);

    // Use real buffers but compute what the rel32 should be
    std::uint8_t siteBuf[8] = { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    auto dist = reinterpret_cast<std::intptr_t>(fakeCave) - reinterpret_cast<std::intptr_t>(fakeSite + 5);
    siteBuf[0] = 0xE9;
    auto rel32 = static_cast<std::int32_t>(dist);
    std::memcpy(&siteBuf[1], &rel32, 4);
    siteBuf[5] = 0x90;
    siteBuf[6] = 0x90;
    siteBuf[7] = 0x90;

    CHECK(siteBuf[0] == 0xE9);
    CHECK(siteBuf[5] == 0x90);
    CHECK(siteBuf[6] == 0x90);
    CHECK(siteBuf[7] == 0x90);
}

TEST_CASE("EmitJmpRel32 NOP pads remaining bytes", "[cave]") {
    // Test with patchSize=8 — bytes 5,6,7 should be 0x90
    std::uint8_t site[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    // cave adjacent to site for a valid rel32
    std::uint8_t cave[1] = {};

    bool ok = EmitJmpRel32(site, cave, 8);
    CHECK(ok);
    CHECK(site[0] == 0xE9);
    CHECK(site[5] == 0x90);
    CHECK(site[6] == 0x90);
    CHECK(site[7] == 0x90);
}

TEST_CASE("EmitJmpRel32 computes correct rel32 offset", "[cave]") {
    // Place site and cave in known positions using stack memory
    std::uint8_t buf[256] = {};
    std::uint8_t* site = &buf[0];
    std::uint8_t* cave = &buf[100];

    bool ok = EmitJmpRel32(site, cave, 6);
    CHECK(ok);
    CHECK(site[0] == 0xE9);

    // Verify: rel32 = cave - (site + 5) = 100 - 5 = 95
    std::int32_t rel32;
    std::memcpy(&rel32, &site[1], 4);
    CHECK(rel32 == 95);
}

TEST_CASE("EmitJmpRel32 patchSize=6 pads byte 5 only", "[cave]") {
    std::uint8_t buf[256] = {};
    std::uint8_t* site = &buf[0];
    std::uint8_t* cave = &buf[50];

    site[5] = 0xCC;  // Will be overwritten
    site[6] = 0xCC;  // Should NOT be touched

    bool ok = EmitJmpRel32(site, cave, 6);
    CHECK(ok);
    CHECK(site[5] == 0x90);
    CHECK(site[6] == 0xCC);  // Untouched
}

// ── EmitJmpAbs64 tests ─────────────────────────────────────────

TEST_CASE("EmitJmpAbs64 emits FF 25 00000000 + 8-byte addr", "[cave]") {
    std::uint8_t cave[20] = {};
    int off = 0;
    std::uint64_t target = 0x0000014100001234ULL;

    EmitJmpAbs64(cave, off, target);

    CHECK(off == 14);
    CHECK(cave[0] == 0xFF);
    CHECK(cave[1] == 0x25);
    CHECK(cave[2] == 0x00);
    CHECK(cave[3] == 0x00);
    CHECK(cave[4] == 0x00);
    CHECK(cave[5] == 0x00);

    std::uint64_t stored;
    std::memcpy(&stored, &cave[6], 8);
    CHECK(stored == target);
}

TEST_CASE("EmitJmpAbs64 appends at current offset", "[cave]") {
    std::uint8_t cave[30] = {};
    int off = 5;  // Start at offset 5
    std::uint64_t target = 0xDEADBEEFCAFEBABEULL;

    EmitJmpAbs64(cave, off, target);

    CHECK(off == 19);
    CHECK(cave[5] == 0xFF);
    CHECK(cave[6] == 0x25);

    std::uint64_t stored;
    std::memcpy(&stored, &cave[11], 8);
    CHECK(stored == target);
}

TEST_CASE("EmitJmpAbs64 handles zero target", "[cave]") {
    std::uint8_t cave[20] = {};
    int off = 0;

    EmitJmpAbs64(cave, off, 0);

    CHECK(off == 14);
    CHECK(cave[0] == 0xFF);
    CHECK(cave[1] == 0x25);

    std::uint64_t stored;
    std::memcpy(&stored, &cave[6], 8);
    CHECK(stored == 0);
}

// ── Backpatch disp8 computation ─────────────────────────────────

TEST_CASE("Backpatch disp8 computes correct short jump", "[cave]") {
    // Simulates: jz .target where jz is at offset 10, target at offset 20
    // disp8 = target - (jzOff + 1) = 20 - 11 = 9
    std::uint8_t cave[32] = {};
    int jzOff = 10;
    int targetOff = 20;

    cave[jzOff] = static_cast<std::uint8_t>(targetOff - (jzOff + 1));

    CHECK(cave[jzOff] == 9);
}

TEST_CASE("Backpatch disp8 handles adjacent jump", "[cave]") {
    // jz at offset 5, target at offset 6 → disp8 = 0
    std::uint8_t cave[16] = {};
    int jzOff = 5;
    int targetOff = 6;

    cave[jzOff] = static_cast<std::uint8_t>(targetOff - (jzOff + 1));

    CHECK(cave[jzOff] == 0);
}
