// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// The semantics, run through the interpreter, against Unicorn at EL0: random encodings of each form
// on random states must leave the same state, and encodings Biwa rejects must fault in Unicorn too
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
#include "biwa/a64/a64_sem.h"
#include "biwa/a64/decoder.h"
#include "biwa/a64/interpreter.h"

namespace {

using biwa::a64::Op;
using biwa::a64::StepResult;
using biwa::test::CpuState;
using biwa::test::UnicornCpu;

constexpr int kSamples = 2000;

bool HasField(Op op, char letter) { return std::string_view(biwa::a64::Info(op).pattern).contains(letter); }

// System registers as MRS and MSR encode them in bits 19..5: op0 (bit 0 only), op1, CRn, CRm, op2.
constexpr std::uint32_t SystemRegister(std::uint32_t op0, std::uint32_t op1, std::uint32_t crn, std::uint32_t crm,
                                       std::uint32_t op2) {
    return ((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}

constexpr std::uint32_t kFpcr = SystemRegister(3, 3, 4, 4, 0);
constexpr std::uint32_t kCntpct = SystemRegister(3, 3, 14, 0, 1);
constexpr std::uint32_t kCntvct = SystemRegister(3, 3, 14, 0, 2);

// The registers EL0 may use, and some it may not; random fields almost never hit them.
constexpr std::array kSystemRegisters = {
    SystemRegister(3, 3, 0, 0, 1),  SystemRegister(3, 3, 0, 0, 7),
    SystemRegister(3, 3, 4, 2, 0),  kFpcr,
    SystemRegister(3, 3, 4, 4, 1),  SystemRegister(3, 3, 9, 14, 0),
    SystemRegister(3, 3, 13, 0, 2), SystemRegister(3, 3, 13, 0, 3),
    SystemRegister(3, 3, 14, 0, 0), SystemRegister(3, 0, 1, 0, 0),
    SystemRegister(3, 0, 4, 1, 0),  SystemRegister(3, 3, 4, 2, 1),
    SystemRegister(3, 0, 4, 2, 2),  SystemRegister(2, 3, 0, 1, 0),
    SystemRegister(3, 3, 14, 3, 1),
};

// A random encoding of `op`; for MRS and MSR, three times in four a register from the list above.
std::uint32_t RandomWord(Op op, std::mt19937_64& rng) {
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    std::uint32_t word = (static_cast<std::uint32_t>(rng()) & ~info.mask) | info.value;
    if ((op == Op::MRS || op == Op::MSR_reg) && rng() % 4 != 0) {
        word = (word & ~(0x7FFFu << 5)) | (kSystemRegisters[rng() % kSystemRegisters.size()] << 5);
    }
    return word;
}

// Adjusts `in` for what Unicorn does not model like the console: it keeps FPCR's trap enables and
// FZ16, which the A57 does not have.
void AdjustForUnicorn(Op op, std::uint32_t word, CpuState& in) {
    if (op == Op::MSR_reg && ((word >> 5) & 0x7FFF) == kFpcr && (word & 31) != 31) {
        in.x[word & 31] &= SEM_FPCR_MASK;
    }
}

// Whether a sample is left out: the counters come from the runtime, which the interpreter does not
// have yet.
bool Skipped(Op op, std::uint32_t word) {
    const std::uint32_t reg = (word >> 5) & 0x7FFF;
    return op == Op::MRS && (reg == kCntpct || reg == kCntvct);
}

// Counts of a form's samples.
struct Tally {
    int run = 0;
    int exceptions = 0;  // undefined, SVC, BRK
};

// Compares one step of Biwa with Unicorn's run of the same word from the same state.
void Compare(const biwa::a64::OpInfo& info, std::uint32_t word, StepResult result, const BiwaCpu& cpu,
             const UnicornCpu::Outcome& reference, Tally& tally) {
    ++tally.run;
    if (result == StepResult::kUndefined || result == StepResult::kSupervisorCall ||
        result == StepResult::kBreakpoint) {
        ++tally.exceptions;
        EXPECT_FALSE(reference.completed) << std::format("{:08x}: Biwa takes an exception, Unicorn runs it", word);
        return;
    }
    ASSERT_EQ(result, StepResult::kOk) << std::format("{:08x}", word);
    ASSERT_TRUE(reference.completed) << std::format("{:08x}: Unicorn: {}", word, reference.error);
    const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
    ASSERT_EQ(diff, "") << std::format("{:08x} ({}), Unicorn != Biwa:\n", word, info.name) << diff;
}

std::uint64_t Seed(Op op) { return static_cast<std::uint64_t>(op) * 7919 + 1; }

class SemanticsTest : public testing::TestWithParam<Op> {};

TEST_P(SemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    UnicornCpu unicorn;
    std::mt19937_64 rng(Seed(op));
    Tally tally;
    for (int tries = 0; tally.run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = RandomWord(op, rng);
        if (biwa::a64::Decode(word) != op || Skipped(op, word)) {
            continue;  // a more specific form owns this encoding
        }
        CpuState in = biwa::test::RandomState(rng);
        in.sp = rng() & ~std::uint64_t{0xF};
        in.pc = UnicornCpu::kCodeAddress;
        AdjustForUnicorn(op, word, in);
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        const StepResult result = biwa::a64::Step(cpu, word);
        Compare(info, word, result, cpu, unicorn.Run(in, std::array<std::uint32_t, 1>{word}), tally);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_EQ(tally.run, kSamples);
    EXPECT_LT(tally.exceptions, tally.run);
}

// Forms that always take an exception at EL0: SVC and BRK, and the forms that are privileged or
// from later architecture versions (undefined on the console).
class ExceptionTest : public testing::TestWithParam<Op> {};

TEST_P(ExceptionTest, BothTakeIt) {
    const Op op = GetParam();
    const StepResult expected = op == Op::SVC   ? StepResult::kSupervisorCall
                                : op == Op::BRK ? StepResult::kBreakpoint
                                                : StepResult::kUndefined;
    UnicornCpu unicorn;
    std::mt19937_64 rng(Seed(op));
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t word = RandomWord(op, rng);
        CpuState in = biwa::test::RandomState(rng);
        in.pc = UnicornCpu::kCodeAddress;
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        EXPECT_EQ(biwa::a64::Step(cpu, word), expected) << std::format("{:08x}", word);
        EXPECT_EQ(cpu.pc, in.pc + (op == Op::SVC ? 4 : 0));  // SVC returns to the next instruction
        EXPECT_FALSE(unicorn.Run(in, std::array<std::uint32_t, 1>{word}).completed) << std::format("{:08x}", word);
    }
}

// Guest memory for the load, store and branch tests. Guest addresses are host addresses, so the
// block is host memory that Unicorn maps at the same address.
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

// A random encoding whose addresses stay in the block: pc-relative offsets (literals, branches)
// are cut to +-16 KiB; the instruction sits in the middle of the block.
std::uint32_t RandomAccess(Op op, std::mt19937_64& rng) {
    std::uint32_t word = RandomWord(op, rng);
    const std::string_view pattern = biwa::a64::Info(op).pattern;
    const std::size_t first = pattern.find('i');
    if (first != std::string_view::npos && !HasField(op, 'n')) {
        const std::size_t last = pattern.rfind('i');
        const auto low = static_cast<std::uint32_t>(31 - last);
        const auto width = static_cast<std::uint32_t>(last - first + 1);
        const std::uint32_t field = ((1u << width) - 1) << low;
        std::uint32_t imm = static_cast<std::uint32_t>(rng()) & 0xFFF;
        if (rng() % 2 != 0) {
            imm = ~imm;  // negative
        }
        word = (word & ~field) | ((imm << low) & field);
    }
    return word;
}

// The register that holds an address: Rn, or Rt for DC ZVA.
char AddressRegister(Op op) { return op == Op::DC_ZVA ? 't' : 'n'; }

class MemorySemanticsTest : public testing::TestWithParam<Op> {};

// As SemanticsTest, with the address register pointing into a block of random memory, pc inside
// it, and the index register small (negative too for SXTW / SXTX); the block must end up the same
// as Unicorn's.
TEST_P(MemorySemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    std::mt19937_64 rng(Seed(op));
    GuestBlock block;
    for (std::size_t i = 0; i < GuestBlock::kSize; i += 8) {
        const std::uint64_t value = rng();
        std::memcpy(&block.Bytes()[i], &value, 8);
    }
    UnicornCpu unicorn(block.Address(), GuestBlock::kSize);
    unicorn.WriteData(0, block.Bytes());
    std::vector<std::uint8_t> reference_memory(GuestBlock::kSize);
    const char address_letter = AddressRegister(op);
    const bool has_base = HasField(op, address_letter);
    const bool has_index = HasField(op, 'm');
    Tally tally;
    for (int tries = 0; tally.run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = RandomAccess(op, rng);
        if (biwa::a64::Decode(word) != op) {
            continue;
        }
        CpuState in = biwa::test::RandomState(rng);
        const std::uint64_t base = block.Address() + GuestBlock::kBaseOffset + (rng() % 0x2000) - 0x1000;
        in.sp = base & ~std::uint64_t{0xF};
        in.pc = block.Address() + GuestBlock::kCodeOffset + 4 * (rng() % 0x100);
        const std::uint32_t n = biwa::a64::Field(op, word, address_letter);
        if (has_base && n != 31) {
            in.x[n] = (rng() % 4 == 0) ? base & ~std::uint64_t{0xF} : base;
        }
        const bool is_branch = op == Op::BR || op == Op::BLR || op == Op::RET;
        if ((is_branch || address_letter == 't') && n == 31) {
            continue;  // XZR: address 0, which neither the host nor Unicorn has mapped
        }
        if (is_branch) {
            in.x[n] &= ~std::uint64_t{3};  // a misaligned target faults at the target, not here
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
        unicorn.ReadData(0, reference_memory);
        if (std::memcmp(reference_memory.data(), block.Bytes().data(), GuestBlock::kSize) != 0) {
            const auto mismatch = std::ranges::mismatch(reference_memory, block.Bytes());
            FAIL() << std::format("{:08x} ({}): memory differs at block offset {:#x}", word, info.name,
                                  mismatch.in1 - reference_memory.begin());
        }
        Compare(info, word, result, cpu, reference, tally);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_EQ(tally.run, kSamples);
    EXPECT_LT(tally.exceptions, tally.run);
}

std::string Name(const testing::TestParamInfo<Op>& param) { return std::string(biwa::a64::Info(param.param).id); }

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

// Loads and stores of SIMD structures.
INSTANTIATE_TEST_SUITE_P(Structures, MemorySemanticsTest,
                         testing::Values(Op::STx_mult_1, Op::STx_mult_2, Op::LDx_mult_1, Op::LDx_mult_2, Op::ST1_sngl_1,
                                         Op::ST1_sngl_2, Op::ST2_sngl_1, Op::ST2_sngl_2, Op::ST3_sngl_1, Op::ST3_sngl_2,
                                         Op::ST4_sngl_1, Op::ST4_sngl_2, Op::LD1_sngl_1, Op::LD1_sngl_2, Op::LD2_sngl_1,
                                         Op::LD2_sngl_2, Op::LD3_sngl_1, Op::LD3_sngl_2, Op::LD4_sngl_1, Op::LD4_sngl_2,
                                         Op::LD1R_1, Op::LD1R_2, Op::LD2R_1, Op::LD2R_2, Op::LD3R_1, Op::LD3R_2,
                                         Op::LD4R_1, Op::LD4R_2),
                         Name);

// Branches (in the block, so the targets are mapped) and DC ZVA.
INSTANTIATE_TEST_SUITE_P(Branches, MemorySemanticsTest,
                         testing::Values(Op::B_uncond, Op::BL, Op::B_cond, Op::CBZ, Op::CBNZ, Op::TBZ, Op::TBNZ, Op::BR,
                                         Op::BLR, Op::RET, Op::DC_ZVA),
                         Name);

// System instructions that run at EL0.
INSTANTIATE_TEST_SUITE_P(System, SemanticsTest,
                         testing::Values(Op::HINT, Op::NOP, Op::YIELD, Op::WFE, Op::WFI, Op::SEV, Op::SEVL, Op::DSB,
                                         Op::DMB, Op::ISB, Op::MRS, Op::MSR_reg, Op::DC_CVAC, Op::DC_CVAU, Op::DC_CIVAC,
                                         Op::IC_IVAU),
                         Name);

INSTANTIATE_TEST_SUITE_P(System, ExceptionTest,
                         testing::Values(Op::SVC, Op::BRK, Op::DC_IVAC, Op::DC_ISW, Op::DC_CSW, Op::DC_CISW,
                                         Op::IC_IALLU, Op::IC_IALLUIS, Op::DC_CVAP, Op::CFINV, Op::RMIF, Op::XAFlag,
                                         Op::AXFlag),
                         Name);

}  // namespace
