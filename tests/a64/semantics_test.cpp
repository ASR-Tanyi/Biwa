// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// The semantics, run through the interpreter, against Unicorn: random encodings of each form on
// random states must leave the same state, and encodings Biwa rejects must fault in Unicorn too
// (docs/design/09-testing-and-debugging.md, "Instruction semantics").

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "a64/cpu_state.h"
#include "a64/unicorn_cpu.h"
#include "biwa/a64/decoder.h"
#include "biwa/a64/interpreter.h"

namespace {

using biwa::a64::Op;
using biwa::a64::StepResult;
using biwa::test::CpuState;
using biwa::test::UnicornCpu;

constexpr int kSamples = 2000;

class SemanticsTest : public testing::TestWithParam<Op> {};

TEST_P(SemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    UnicornCpu unicorn;
    std::mt19937_64 rng(static_cast<std::uint64_t>(op) * 7919 + 1);
    int run = 0;
    int undefined = 0;
    for (int tries = 0; run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = (static_cast<std::uint32_t>(rng()) & ~info.mask) | info.value;
        if (biwa::a64::Decode(word) != op) {
            continue;  // a more specific form owns this encoding
        }
        CpuState in = biwa::test::RandomState(rng);
        in.sp = rng() & ~std::uint64_t{0xF};
        in.pc = UnicornCpu::kCodeAddress;
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        const StepResult result = biwa::a64::Step(cpu, word);
        const UnicornCpu::Outcome reference = unicorn.Run(in, std::array<std::uint32_t, 1>{word});
        ++run;
        if (result == StepResult::kUndefined) {
            ++undefined;
            EXPECT_FALSE(reference.completed) << std::format("{:08x}: Biwa rejects it, Unicorn runs it", word);
            continue;
        }
        ASSERT_EQ(result, StepResult::kOk) << std::format("{:08x}", word);
        ASSERT_TRUE(reference.completed) << std::format("{:08x}: Unicorn: {}", word, reference.error);
        const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
        ASSERT_EQ(diff, "") << std::format("{:08x} ({}), Unicorn != Biwa:\n", word, info.name) << diff;
    }
    EXPECT_EQ(run, kSamples);
    EXPECT_LT(undefined, run);
}

std::string Name(const testing::TestParamInfo<Op>& param) { return std::string(biwa::a64::Info(param.param).id); }

// Guest memory for the load and store tests. Guest addresses are host addresses, so the block is
// host memory that Unicorn maps at the same address.
class GuestBlock {
   public:
    static constexpr std::size_t kSize = 0x20000;
    // Where the tests put base registers (with up to 4 KiB either side) and the instruction.
    static constexpr std::uint64_t kBaseOffset = 0x9000;
    static constexpr std::uint64_t kCodeOffset = 0x10000;

    GuestBlock() {
        void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            throw std::runtime_error("mmap failed");
        }
        data_ = static_cast<std::uint8_t*>(p);
    }
    ~GuestBlock() { munmap(data_, kSize); }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    [[nodiscard]] std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data_); }
    [[nodiscard]] std::span<std::uint8_t> Bytes() const { return {data_, kSize}; }

   private:
    std::uint8_t* data_ = nullptr;
};

bool HasField(Op op, char letter) { return std::string_view(biwa::a64::Info(op).pattern).contains(letter); }

// A random encoding of a load or store whose address stays in the block: literal offsets are cut
// to +-16 KiB (the instruction sits in the middle of the block).
std::uint32_t RandomAccess(Op op, std::mt19937_64& rng) {
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    std::uint32_t word = (static_cast<std::uint32_t>(rng()) & ~info.mask) | info.value;
    if (!HasField(op, 'n')) {
        const std::uint32_t imm = (word >> 5) & 0x1FFF;
        const std::uint32_t imm19 = (imm & 0x1000) != 0 ? imm | 0x7E000 : imm;
        word = (word & ~(0x7FFFFu << 5)) | (imm19 << 5);
    }
    return word;
}

class MemorySemanticsTest : public testing::TestWithParam<Op> {};

// As SemanticsTest, with the base register pointing into a block of random memory and the index
// register small (negative too for SXTW / SXTX); the block must end up the same as Unicorn's.
TEST_P(MemorySemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    std::mt19937_64 rng(static_cast<std::uint64_t>(op) * 7919 + 1);
    GuestBlock block;
    for (std::size_t i = 0; i < GuestBlock::kSize; i += 8) {
        const std::uint64_t value = rng();
        std::memcpy(&block.Bytes()[i], &value, 8);
    }
    UnicornCpu unicorn(block.Address(), GuestBlock::kSize);
    unicorn.WriteData(0, block.Bytes());
    std::vector<std::uint8_t> reference_memory(GuestBlock::kSize);
    const bool has_base = HasField(op, 'n');
    const bool has_index = HasField(op, 'm');
    int run = 0;
    int undefined = 0;
    for (int tries = 0; run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = RandomAccess(op, rng);
        if (biwa::a64::Decode(word) != op) {
            continue;
        }
        CpuState in = biwa::test::RandomState(rng);
        const std::uint64_t base = block.Address() + GuestBlock::kBaseOffset + (rng() % 0x2000) - 0x1000;
        in.sp = base & ~std::uint64_t{0xF};
        in.pc = block.Address() + GuestBlock::kCodeOffset + 4 * (rng() % 0x100);
        const std::uint32_t n = biwa::a64::Field(op, word, 'n');
        if (has_base && n != 31) {
            in.x[n] = (rng() % 4 == 0) ? base & ~std::uint64_t{0xF} : base;
        }
        if (has_index) {
            const std::uint32_t m = biwa::a64::Field(op, word, 'm');
            if (m == n && m != 31) {
                continue;  // the index would be an address (31 is SP as the base, XZR as the index)
            }
            if (m != 31) {
                const std::uint32_t option = HasField(op, 'x') ? biwa::a64::Field(op, word, 'x') : 3;
                std::int64_t index = static_cast<std::int64_t>(rng() % 4096);
                if ((option & 4) != 0) {
                    index -= 2048;  // SXTW, SXTX
                }
                std::uint64_t value = static_cast<std::uint64_t>(index);
                if ((option & 1) == 0) {
                    value = (value & 0xFFFFFFFF) | (rng() << 32);  // UXTW, SXTW read only Wm
                }
                in.x[m] = value;
            }
        }
        const std::uint64_t code_offset = in.pc - block.Address();
        std::memcpy(&block.Bytes()[code_offset], &word, 4);
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        const StepResult result = biwa::a64::Step(cpu, word);
        const UnicornCpu::Outcome reference = unicorn.Run(in, std::array<std::uint32_t, 1>{word});
        ++run;
        unicorn.ReadData(0, reference_memory);
        if (std::memcmp(reference_memory.data(), block.Bytes().data(), GuestBlock::kSize) != 0) {
            const auto mismatch = std::ranges::mismatch(reference_memory, block.Bytes());
            FAIL() << std::format("{:08x} ({}): memory differs at block offset {:#x}", word, info.name,
                                  mismatch.in1 - reference_memory.begin());
        }
        if (result == StepResult::kUndefined) {
            ++undefined;
            EXPECT_FALSE(reference.completed) << std::format("{:08x}: Biwa rejects it, Unicorn runs it", word);
            continue;
        }
        ASSERT_EQ(result, StepResult::kOk) << std::format("{:08x}", word);
        ASSERT_TRUE(reference.completed) << std::format("{:08x}: Unicorn: {}", word, reference.error);
        const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
        ASSERT_EQ(diff, "") << std::format("{:08x} ({}), Unicorn != Biwa:\n", word, info.name) << diff;
    }
    EXPECT_EQ(run, kSamples);
    EXPECT_LT(undefined, run);
}

// Data processing: immediate.
INSTANTIATE_TEST_SUITE_P(Immediate, SemanticsTest,
                         testing::Values(Op::ADR, Op::ADRP, Op::ADD_imm, Op::ADDS_imm, Op::SUB_imm, Op::SUBS_imm,
                                         Op::AND_imm, Op::ORR_imm, Op::EOR_imm, Op::ANDS_imm, Op::MOVN, Op::MOVZ,
                                         Op::MOVK, Op::SBFM, Op::BFM, Op::UBFM, Op::ASR_1, Op::ASR_2, Op::SXTB_1,
                                         Op::SXTB_2, Op::SXTH_1, Op::SXTH_2, Op::SXTW, Op::EXTR),
                         Name);

// Data processing: register.
INSTANTIATE_TEST_SUITE_P(Register, SemanticsTest,
                         testing::Values(Op::AND_shift, Op::BIC_shift, Op::ORR_shift, Op::ORN_shift, Op::EOR_shift,
                                         Op::EON, Op::ANDS_shift, Op::BICS, Op::ADD_shift, Op::ADDS_shift,
                                         Op::SUB_shift, Op::SUBS_shift, Op::ADD_ext, Op::ADDS_ext, Op::SUB_ext,
                                         Op::SUBS_ext, Op::RBIT_int, Op::REV16_int, Op::REV, Op::REV32_int, Op::CLZ_int,
                                         Op::CLS_int, Op::UDIV, Op::SDIV, Op::LSLV, Op::LSRV, Op::ASRV, Op::RORV,
                                         Op::CRC32, Op::CRC32C, Op::ADC, Op::ADCS, Op::SBC, Op::SBCS, Op::CCMN_reg,
                                         Op::CCMP_reg, Op::CCMN_imm, Op::CCMP_imm, Op::CSEL, Op::CSINC, Op::CSINV,
                                         Op::CSNEG, Op::MADD, Op::MSUB, Op::SMADDL, Op::SMSUBL, Op::SMULH, Op::UMADDL,
                                         Op::UMSUBL, Op::UMULH),
                         Name);

// Loads and stores on single registers and pairs.
INSTANTIATE_TEST_SUITE_P(LoadStore, MemorySemanticsTest,
                         testing::Values(Op::LDR_lit_gen, Op::LDRSW_lit, Op::PRFM_lit, Op::LDR_lit_fpsimd,
                                         Op::STNP_LDNP_gen, Op::STNP_LDNP_fpsimd, Op::STP_LDP_gen, Op::STP_LDP_fpsimd,
                                         Op::STURx_LDURx, Op::PRFM_imm, Op::PRFM_unscaled_imm, Op::STUR_fpsimd,
                                         Op::LDUR_fpsimd, Op::STRx_LDRx_imm_1, Op::STRx_LDRx_imm_2,
                                         Op::STR_imm_fpsimd_1, Op::STR_imm_fpsimd_2, Op::LDR_imm_fpsimd_1,
                                         Op::LDR_imm_fpsimd_2, Op::STTRB, Op::LDTRB, Op::LDTRSB, Op::STTRH, Op::LDTRH,
                                         Op::LDTRSH, Op::STTR, Op::LDTR, Op::LDTRSW, Op::STRx_reg, Op::LDRx_reg,
                                         Op::STR_reg_fpsimd, Op::LDR_reg_fpsimd),
                         Name);

}  // namespace
