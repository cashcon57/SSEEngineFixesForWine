#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstddef>

// Extract x86_instr_len as a standalone function for testing.
// This is a copy of the implementation from form_caching_veh.h,
// kept in sync by CI (any divergence = build failure on real target).
namespace {
    std::size_t x86_instr_len(const std::uint8_t* ip)
    {
        const auto* start = ip;

        bool hasRex = false;
        if ((*ip & 0xF0) == 0x40) { hasRex = true; ++ip; }

        bool has66 = false;
        while (*ip == 0x66 || *ip == 0xF0 || *ip == 0xF2 || *ip == 0xF3 ||
               *ip == 0x2E || *ip == 0x3E || *ip == 0x26 ||
               *ip == 0x36 || *ip == 0x64 || *ip == 0x65) {
            if (*ip == 0x66) has66 = true;
            ++ip;
        }

        const auto op = *ip++;
        bool isTwoByteOp = false;

        if (op == 0x0F) {
            isTwoByteOp = true;
            const auto op2 = *ip++;
            if (op2 == 0x38 || op2 == 0x3A) ++ip;
        }

        const auto modrm = *ip++;
        const auto mod   = (modrm >> 6) & 0x3;
        const auto reg   = (modrm >> 3) & 0x7;
        const auto rm    = modrm & 0x7;

        if (mod != 3) {
            if (rm == 4) ++ip;            // SIB byte
            if      (mod == 0 && rm == 5) ip += 4; // RIP-relative disp32
            else if (mod == 1)            ip += 1; // disp8
            else if (mod == 2)            ip += 4; // disp32
        }

        if (!isTwoByteOp) {
            if (op == 0xF6 && reg == 0) ip += 1;
            else if (op == 0xF7 && reg == 0) ip += (has66 ? 2 : 4);
            else if (op == 0x80 || op == 0x82) ip += 1;
            else if (op == 0x81) ip += (has66 ? 2 : 4);
            else if (op == 0x83) ip += 1;
            else if (op == 0xC6 && reg == 0) ip += 1;
            else if (op == 0xC7 && reg == 0) ip += (has66 ? 2 : 4);
            else if (op == 0x69) ip += (has66 ? 2 : 4);
            else if (op == 0x6B) ip += 1;
        }

        return static_cast<std::size_t>(ip - start);
    }
}

// ── Basic ModRM instructions (memory operands) ──────────────────

TEST_CASE("mov eax, [rsi+0x94] — mod=10, disp32", "[x86]") {
    // 8B 86 94 00 00 00
    const std::uint8_t code[] = { 0x8B, 0x86, 0x94, 0x00, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

TEST_CASE("sub eax, [rsi+0x98] — mod=10, disp32", "[x86]") {
    // 2B 86 98 00 00 00
    const std::uint8_t code[] = { 0x2B, 0x86, 0x98, 0x00, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

TEST_CASE("cmp byte [rax+0x1A], 0x2B — disp8 + imm8", "[x86]") {
    // 80 78 1A 2B
    const std::uint8_t code[] = { 0x80, 0x78, 0x1A, 0x2B };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("test byte [rax+0x40], 0x01 — F6 /0 disp8 + imm8", "[x86]") {
    // F6 40 40 01
    const std::uint8_t code[] = { 0xF6, 0x40, 0x40, 0x01 };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("mov eax, [r14 + rax*8] — SIB byte", "[x86]") {
    // 43 8B 04 C6 (REX.XB + mov eax, [r14+rax*8])
    const std::uint8_t code[] = { 0x43, 0x8B, 0x04, 0xC6 };
    CHECK(x86_instr_len(code) == 4);
}

// ── REX-prefixed instructions ───────────────────────────────────

TEST_CASE("mov rdx, rsi — REX.W + reg-reg", "[x86]") {
    // 48 89 F2
    const std::uint8_t code[] = { 0x48, 0x89, 0xF2 };
    CHECK(x86_instr_len(code) == 3);
}

TEST_CASE("mov rdx, [rdx+0x10] — REX.W + disp8", "[x86]") {
    // 48 8B 52 10
    const std::uint8_t code[] = { 0x48, 0x8B, 0x52, 0x10 };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("test rsi, rsi — REX.W + reg-reg", "[x86]") {
    // 48 85 F6
    const std::uint8_t code[] = { 0x48, 0x85, 0xF6 };
    CHECK(x86_instr_len(code) == 3);
}

TEST_CASE("mov r10, rsi — REX.WR", "[x86]") {
    // 49 89 F2
    const std::uint8_t code[] = { 0x49, 0x89, 0xF2 };
    CHECK(x86_instr_len(code) == 3);
}

TEST_CASE("shr r10, 32 — REX.WB + imm8", "[x86]") {
    // 49 C1 EA 20
    const std::uint8_t code[] = { 0x49, 0xC1, 0xEA, 0x20 };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("cmp r10d, 0x7F — REX.B + imm8", "[x86]") {
    // 41 83 FA 7F
    const std::uint8_t code[] = { 0x41, 0x83, 0xFA, 0x7F };
    CHECK(x86_instr_len(code) == 4);
}

// ── Two-byte opcodes (0F xx) ────────────────────────────────────

TEST_CASE("movzx eax, byte [rdx+0x44] — 0F B6 disp8", "[x86]") {
    // 0F B6 42 44
    const std::uint8_t code[] = { 0x0F, 0xB6, 0x42, 0x44 };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("jz rel32 — 0F 84 + imm32", "[x86]") {
    // 0F 84 00 01 00 00
    const std::uint8_t code[] = { 0x0F, 0x84, 0x00, 0x01, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

// ── RIP-relative addressing (mod=00, rm=5) ──────────────────────

TEST_CASE("mov eax, [rip+disp32] — RIP-relative", "[x86]") {
    // 8B 05 xx xx xx xx
    const std::uint8_t code[] = { 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40 };
    CHECK(x86_instr_len(code) == 6);
}

TEST_CASE("cmp [rip+disp32], eax — RIP-relative", "[x86]") {
    // 39 05 xx xx xx xx
    const std::uint8_t code[] = { 0x39, 0x05, 0x10, 0x20, 0x30, 0x40 };
    CHECK(x86_instr_len(code) == 6);
}

// ── SIB + displacement combos ───────────────────────────────────

TEST_CASE("mov [rsp+0x20], rax — SIB + disp8", "[x86]") {
    // 48 89 44 24 20
    const std::uint8_t code[] = { 0x48, 0x89, 0x44, 0x24, 0x20 };
    CHECK(x86_instr_len(code) == 5);
}

TEST_CASE("mov eax, [rsp] — SIB no disp", "[x86]") {
    // 8B 04 24
    const std::uint8_t code[] = { 0x8B, 0x04, 0x24 };
    CHECK(x86_instr_len(code) == 3);
}

// ── Immediate operand variants ──────────────────────────────────

TEST_CASE("cmp dword [rcx+0x10], imm32 — 81 /7 disp8 + imm32", "[x86]") {
    // 81 79 10 FF FF 00 00
    const std::uint8_t code[] = { 0x81, 0x79, 0x10, 0xFF, 0xFF, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 7);
}

TEST_CASE("cmp byte [rcx+0x10], imm8 — 80 /7 disp8 + imm8", "[x86]") {
    // 80 79 10 2B
    const std::uint8_t code[] = { 0x80, 0x79, 0x10, 0x2B };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("imul eax, [rcx], imm32 — 69 disp0 + imm32", "[x86]") {
    // 69 01 10 00 00 00
    const std::uint8_t code[] = { 0x69, 0x01, 0x10, 0x00, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

TEST_CASE("imul eax, [rcx], imm8 — 6B disp0 + imm8", "[x86]") {
    // 6B 01 10
    const std::uint8_t code[] = { 0x6B, 0x01, 0x10 };
    CHECK(x86_instr_len(code) == 3);
}

TEST_CASE("mov byte [rcx], imm8 — C6 /0", "[x86]") {
    // C6 01 42
    const std::uint8_t code[] = { 0xC6, 0x01, 0x42 };
    CHECK(x86_instr_len(code) == 3);
}

TEST_CASE("mov dword [rcx], imm32 — C7 /0", "[x86]") {
    // C7 01 42 00 00 00
    const std::uint8_t code[] = { 0xC7, 0x01, 0x42, 0x00, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

// ── Prefix combinations ─────────────────────────────────────────

TEST_CASE("66 prefix — operand size override + imm16", "[x86]") {
    // 66 81 F9 FF 00 (cmp cx, 0x00FF — mod=11 + imm16)
    const std::uint8_t code[] = { 0x66, 0x81, 0xF9, 0xFF, 0x00 };
    CHECK(x86_instr_len(code) == 5);
}

TEST_CASE("LOCK prefix + cmpxchg", "[x86]") {
    // F0 0F B1 11 (lock cmpxchg [rcx], edx)
    const std::uint8_t code[] = { 0xF0, 0x0F, 0xB1, 0x11 };
    CHECK(x86_instr_len(code) == 4);
}

// ── Real crash-site instructions from form_caching patches ──────

TEST_CASE("call [rax+0x4B8] — PatchA site", "[x86][crash]") {
    // FF 90 B8 04 00 00
    const std::uint8_t code[] = { 0xFF, 0x90, 0xB8, 0x04, 0x00, 0x00 };
    CHECK(x86_instr_len(code) == 6);
}

TEST_CASE("cmp [rdi], eax — PatchE site", "[x86][crash]") {
    // 39 07
    const std::uint8_t code[] = { 0x39, 0x07 };
    CHECK(x86_instr_len(code) == 2);
}

TEST_CASE("mov rax, [rdi]; call [rax+0x08] — PatchG site pair", "[x86][crash]") {
    // 48 8B 07 (mov rax, [rdi])
    const std::uint8_t code1[] = { 0x48, 0x8B, 0x07 };
    CHECK(x86_instr_len(code1) == 3);
    // FF 50 08 (call [rax+0x08])
    const std::uint8_t code2[] = { 0xFF, 0x50, 0x08 };
    CHECK(x86_instr_len(code2) == 3);
}

TEST_CASE("mov rbp, [rbx+0x20] — PatchL site", "[x86][crash]") {
    // 48 8B 6B 20
    const std::uint8_t code[] = { 0x48, 0x8B, 0x6B, 0x20 };
    CHECK(x86_instr_len(code) == 4);
}

TEST_CASE("cmp qword [rcx+8], 0 — PatchN site", "[x86][crash]") {
    // 48 83 79 08 00
    const std::uint8_t code[] = { 0x48, 0x83, 0x79, 0x08, 0x00 };
    CHECK(x86_instr_len(code) == 5);
}
