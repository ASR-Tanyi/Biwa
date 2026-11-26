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
#include <numeric>
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

// The state a failing sample started from, as far as the FP forms read it.
std::string Inputs(std::uint32_t word, const CpuState& in) {
    std::string out = std::format("fpcr {:#x} fpsr {:#x} nzcv {:#x}", in.fpcr, in.fpsr, in.nzcv);
    for (const std::uint32_t r : {(word >> 5) & 31, (word >> 16) & 31, (word >> 10) & 31}) {
        std::uint64_t low = 0;
        std::memcpy(&low, in.v[r].data(), 8);
        out += std::format(", v{} {:016x}", r, low);
    }
    return out;
}

// Compares one step of Biwa with Unicorn's run of the same word from the same state.
void Compare(const biwa::a64::OpInfo& info, std::uint32_t word, StepResult result, const BiwaCpu& cpu,
             const UnicornCpu::Outcome& reference, Tally& tally, const CpuState& in) {
    ++tally.run;
    if (result == StepResult::kUndefined || result == StepResult::kSupervisorCall ||
        result == StepResult::kBreakpoint || result == StepResult::kAlignmentFault) {
        ++tally.exceptions;
        EXPECT_FALSE(reference.completed) << std::format("{:08x}: Biwa takes an exception, Unicorn runs it", word);
        return;
    }
    ASSERT_EQ(result, StepResult::kOk) << std::format("{:08x}", word);
    ASSERT_TRUE(reference.completed) << std::format("{:08x}: Unicorn: {}", word, reference.error);
    const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
    ASSERT_EQ(diff, "") << std::format("{:08x} ({}) from {}, Unicorn != Biwa:\n", word, info.name, Inputs(word, in))
                        << diff;
}

std::uint64_t Seed(Op op) { return static_cast<std::uint64_t>(op) * 7919 + 1; }

// One step as guest code runs: the host FP environment set from FPCR first, its raised flags
// folded into FPSR after.
StepResult GuestStep(BiwaCpu& cpu, std::uint32_t word) {
    sem_fp_enter(&cpu);
    const StepResult result = biwa::a64::Step(cpu, word);
    sem_fp_fold(&cpu);
    return result;
}

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
        const StepResult result = GuestStep(cpu, word);
        Compare(info, word, result, cpu, unicorn.Run(in, std::array<std::uint32_t, 1>{word}), tally, in);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_EQ(tally.run, kSamples);
    EXPECT_LT(tally.exceptions, tally.run);
}

// Hand-picked FP operands that random states rarely produce (tininess on either side of the
// smallest normal, invalid operations, signed zeros, NaN rules, saturation, ties, half precision),
// each under several FPCR settings, against Unicorn. Registers: d0 = op(d1, d2) or d3 + d1 * d2
// (s0 ... for single precision); conversions read d1 or x1 and write d0 or x0.
struct FpCase {
    std::uint32_t word;
    std::uint64_t n;
    std::uint64_t m;
    std::uint64_t a;
};

constexpr std::uint32_t kFmaddD = 0x1F420C20;
constexpr std::uint32_t kFmaddS = 0x1F020C20;
constexpr std::uint32_t kFmulD = 0x1E620820;
constexpr std::uint32_t kFmulS = 0x1E220820;
constexpr std::uint32_t kFaddS = 0x1E222820;
constexpr std::uint32_t kFsubD = 0x1E623820;
constexpr std::uint32_t kFdivD = 0x1E621820;
constexpr std::uint32_t kFsqrtD = 0x1E61C020;
constexpr std::uint32_t kFmaxD = 0x1E624820;
constexpr std::uint32_t kFminD = 0x1E625820;
constexpr std::uint32_t kFmaxnmD = 0x1E626820;
constexpr std::uint32_t kFnmulD = 0x1E628820;
constexpr std::uint32_t kFcvtzsWD = 0x1E780020;
constexpr std::uint32_t kFcvtzsXD = 0x9E780020;
constexpr std::uint32_t kFcvtzuWD = 0x1E790020;
constexpr std::uint32_t kFcvtnsXD = 0x9E600020;
constexpr std::uint32_t kFcvtasWS = 0x1E240020;
constexpr std::uint32_t kFcvtmsWD = 0x1E700020;
constexpr std::uint32_t kFcvtpuWD = 0x1E690020;
constexpr std::uint32_t kFcvtzsWD32 = 0x1E588020;  // fixed point, 32 fraction bits
constexpr std::uint32_t kScvtfDX = 0x9E620020;
constexpr std::uint32_t kUcvtfSX = 0x9E230020;
constexpr std::uint32_t kUcvtfDW3 = 0x1E43F420;  // fixed point, 3 fraction bits
constexpr std::uint32_t kFrintaD = 0x1E664020;
constexpr std::uint32_t kFrintxD = 0x1E674020;
constexpr std::uint32_t kFcvtHS = 0x1E23C020;
constexpr std::uint32_t kFcvtSH = 0x1EE24020;
constexpr std::uint32_t kFcvtSD = 0x1E624020;
constexpr std::uint32_t kFcvtHD = 0x1E63C020;
constexpr std::uint32_t kFcvtDH = 0x1EE2C020;

constexpr std::array kFpCases = {
    // Just below the smallest normal before rounding, rounded up to it: tiny on ARM, not on x86.
    FpCase{kFmaddD, 0x9D90000000000000, 0x1D90000000000000, 0x0010000000000000},
    FpCase{kFmaddD, 0x1D90000000000000, 0x1D90000000000000, 0x0010000000000000},
    FpCase{kFmulD, 0x000FFFFFFFFFFFFF, 0x3FF0000000000001, 0},
    FpCase{kFmulS, 0x007FFFFF, 0x3F800001, 0},
    FpCase{kFmulS, 0x00800000, 0x3F7FFFFF, 0},
    FpCase{kFmaddS, 0x8C800000, 0x0C800000, 0x00800000},
    // An exact denormal result, an underflow to zero, an overflow.
    FpCase{kFmulD, 0x0010000000000000, 0x3FE0000000000000, 0},
    FpCase{kFmulD, 0x0010000000000001, 0x3C90000000000000, 0},
    FpCase{kFmulD, 0x7FEFFFFFFFFFFFFF, 0x4000000000000000, 0},
    // Denormal operands (IDC with FZ).
    FpCase{kFaddS, 0x00000001, 0x80400000, 0},
    // Signed zeros, invalid operations, division by zero.
    FpCase{kFsubD, 0x3FF0000000000000, 0x3FF0000000000000, 0},
    FpCase{kFsubD, 0x7FF0000000000000, 0x7FF0000000000000, 0},
    FpCase{kFdivD, 0, 0, 0},
    FpCase{kFdivD, 0x3FF0000000000000, 0x8000000000000000, 0},
    FpCase{kFsqrtD, 0xBFF0000000000000, 0, 0},
    FpCase{kFsqrtD, 0x8000000000000000, 0, 0},
    FpCase{kFnmulD, 0, 0x7FF0000000000000, 0},
    FpCase{kFmaddD, 0x7FF0000000000000, 0, 0x7FF8000000000001},
    FpCase{kFmaddD, 0x7FF0000000000000, 0, 0x7FF0000000000001},
    FpCase{kFmaddS, 0x7FA00000, 0x7FC00001, 0x7FC00002},
    // Max and min of zeros, NaN against a number.
    FpCase{kFmaxD, 0, 0x8000000000000000, 0},
    FpCase{kFminD, 0, 0x8000000000000000, 0},
    FpCase{kFmaxnmD, 0x7FF8000000000005, 0x3FF0000000000000, 0},
    FpCase{kFmaxnmD, 0x7FF0000000000005, 0x3FF0000000000000, 0},
    FpCase{kFmaxD, 0x7FF8000000000005, 0x3FF0000000000000, 0},
    // To integer: saturation at both ends, ties, negative values to unsigned, fixed point.
    FpCase{kFcvtzsWD, 0x41E0000000000000, 0, 0},
    FpCase{kFcvtzsWD, 0x41DFFFFFFFE00000, 0, 0},
    FpCase{kFcvtzsWD, 0xC1E0000000000000, 0, 0},
    FpCase{kFcvtzsWD, 0xC1E0000000200000, 0, 0},
    FpCase{kFcvtzsXD, 0x43E0000000000000, 0, 0},
    FpCase{kFcvtzsXD, 0xC3E0000000000000, 0, 0},
    FpCase{kFcvtzuWD, 0xBFE0000000000000, 0, 0},
    FpCase{kFcvtzuWD, 0xBFF0000000000000, 0, 0},
    FpCase{kFcvtzuWD, 0x41EFFFFFFFF00000, 0, 0},
    FpCase{kFcvtzuWD, 0x41F0000000000000, 0, 0},
    FpCase{kFcvtnsXD, 0x4004000000000000, 0, 0},
    FpCase{kFcvtnsXD, 0x400C000000000000, 0, 0},
    FpCase{kFcvtnsXD, 0xC004000000000000, 0, 0},
    FpCase{kFcvtasWS, 0x40200000, 0, 0},
    FpCase{kFcvtasWS, 0xC0200000, 0, 0},
    FpCase{kFcvtmsWD, 0xBFE0000000000000, 0, 0},
    FpCase{kFcvtpuWD, 0x3FB999999999999A, 0, 0},
    FpCase{kFcvtzsWD32, 0x3FE0000000000000, 0, 0},
    FpCase{kFcvtzsWD32, 0xBFE0000000000000, 0, 0},
    FpCase{kFcvtzsWD, 0x7FF0000000000001, 0, 0},
    // From integer (x1 holds the same bits as d1).
    FpCase{kScvtfDX, 0x0020000000000001, 0, 0},
    FpCase{kScvtfDX, 0x8000000000000001, 0, 0},
    FpCase{kUcvtfSX, 0xFFFFFFFFFFFFFFFF, 0, 0},
    FpCase{kUcvtfSX, 0x0000000001000001, 0, 0},
    FpCase{kUcvtfDW3, 0xFFFFFFFF00000007, 0, 0},
    // Round to integral.
    FpCase{kFrintaD, 0x4004000000000000, 0, 0},
    FpCase{kFrintaD, 0xBFE0000000000000, 0, 0},
    FpCase{kFrintaD, 0xBFD999999999999A, 0, 0},
    FpCase{kFrintxD, 0x4004000000000000, 0, 0},
    FpCase{kFrintxD, 0x0000000000000001, 0, 0},
    // Precision: half overflow and AHP, half tininess, NaNs, the single tininess boundary.
    FpCase{kFcvtHS, 0x477FF000, 0, 0},
    FpCase{kFcvtHS, 0x477FE000, 0, 0},
    FpCase{kFcvtHS, 0x33000000, 0, 0},
    FpCase{kFcvtHS, 0x33400000, 0, 0},
    FpCase{kFcvtHS, 0x7F800000, 0, 0},
    FpCase{kFcvtHS, 0x7FA00001, 0, 0},
    FpCase{kFcvtHS, 0x38800000, 0, 0},
    FpCase{kFcvtSH, 0x7C01, 0, 0},
    FpCase{kFcvtSH, 0x7FFF, 0, 0},
    FpCase{kFcvtSH, 0xFC00, 0, 0},
    FpCase{kFcvtSH, 0x0001, 0, 0},
    FpCase{kFcvtDH, 0x83FF, 0, 0},
    FpCase{kFcvtHD, 0x40EFFE0000000001, 0, 0},
    FpCase{kFcvtHD, 0x7FF0000000000123, 0, 0},
    FpCase{kFcvtSD, 0x36A8000000000000, 0, 0},
    FpCase{kFcvtSD, 0x380FFFFFFF000000, 0, 0},
    FpCase{kFcvtSD, 0x7FF0000000000123, 0, 0},
    FpCase{kFcvtSD, 0x47F0000000000000, 0, 0},
};

// FPCR: nearest, +inf, -inf, zero; with FZ; with DN; AHP, DN and FZ rounding down.
constexpr std::array<std::uint32_t, 8> kFpcrs = {
    0, 1u << 22, 2u << 22, 3u << 22, 1u << 24, (1u << 24) | (1u << 22), 1u << 25, (7u << 24) | (2u << 22)};

TEST(DirectedFpTest, MatchesUnicorn) {
    UnicornCpu unicorn;
    for (const FpCase& c : kFpCases) {
        for (const std::uint32_t fpcr : kFpcrs) {
            CpuState in;
            in.pc = UnicornCpu::kCodeAddress;
            in.fpcr = fpcr;
            for (std::size_t i = 0; i < 4; ++i) {
                const std::uint64_t value = i == 1 ? c.n : i == 2 ? c.m : i == 3 ? c.a : 0x5555555555555555;
                std::memcpy(in.v[i].data(), &value, 8);
            }
            in.x[1] = c.n;
            BiwaCpu cpu = biwa::test::ToBiwa(in);
            ASSERT_EQ(GuestStep(cpu, c.word), StepResult::kOk);
            const UnicornCpu::Outcome reference = unicorn.Run(in, std::array<std::uint32_t, 1>{c.word});
            ASSERT_TRUE(reference.completed);
            EXPECT_EQ(biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu)), "")
                << std::format("{:08x} from {}", c.word, Inputs(c.word, in));
        }
    }
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
        EXPECT_EQ(GuestStep(cpu, word), expected) << std::format("{:08x}", word);
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

// A block of random guest memory, mapped in Unicorn too, for the tests that touch memory or branch.
class MemoryRig {
   public:
    explicit MemoryRig(std::mt19937_64& rng) : unicorn_(Fill(block_, rng), GuestBlock::kSize) {
        unicorn_.WriteData(0, block_.Bytes());
    }

    // A base address: kBaseOffset into the block, up to 4 KiB either side.
    [[nodiscard]] std::uint64_t Base(std::mt19937_64& rng) const {
        return block_.Address() + GuestBlock::kBaseOffset + (rng() % 0x2000) - 0x1000;
    }

    // Where the instructions go.
    [[nodiscard]] std::uint64_t Code(std::mt19937_64& rng) const {
        return block_.Address() + GuestBlock::kCodeOffset + 4 * (rng() % 0x100);
    }

    // Writes `words` at in.pc in the block (Unicorn's copy gets them from Run) and runs Unicorn.
    UnicornCpu::Outcome Run(const CpuState& in, std::span<const std::uint32_t> words) {
        std::memcpy(&block_.Bytes()[in.pc - block_.Address()], words.data(), words.size_bytes());
        return unicorn_.Run(in, words);
    }

    // Where Biwa's block and Unicorn's differ, or "" when they do not.
    std::string MemoryDiff() {
        unicorn_.ReadData(0, reference_);
        if (std::memcmp(reference_.data(), block_.Bytes().data(), GuestBlock::kSize) == 0) {
            return "";
        }
        const auto mismatch = std::ranges::mismatch(reference_, block_.Bytes());
        return std::format("memory differs at block offset {:#x}", mismatch.in1 - reference_.begin());
    }

   private:
    static std::uint64_t Fill(const GuestBlock& block, std::mt19937_64& rng) {
        for (std::size_t i = 0; i < GuestBlock::kSize; i += 8) {
            const std::uint64_t value = rng();
            std::memcpy(&block.Bytes()[i], &value, 8);
        }
        return block.Address();
    }

    GuestBlock block_;
    UnicornCpu unicorn_;
    std::vector<std::uint8_t> reference_ = std::vector<std::uint8_t>(GuestBlock::kSize);
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

bool IsExclusiveOrOrdered(Op op) {
    switch (op) {
        case Op::LDXR:
        case Op::LDAXR:
        case Op::LDXP:
        case Op::LDAXP:
        case Op::STXR:
        case Op::STLXR:
        case Op::STXP:
        case Op::STLXP:
        case Op::LDAR:
        case Op::STLR:
            return true;
        default:
            return false;
    }
}

// The CONSTRAINED UNPREDICTABLE register choices of the exclusives, which compilers never emit:
// the status register also a source or the base, or a load pair into one register twice.
bool Unpredictable(Op op, std::uint32_t word) {
    const std::uint32_t s = (word >> 16) & 31;
    const std::uint32_t t2 = (word >> 10) & 31;
    const std::uint32_t n = (word >> 5) & 31;
    const std::uint32_t t = word & 31;
    switch (op) {
        case Op::STXR:
        case Op::STLXR:
            return s == t || (s == n && n != 31);
        case Op::STXP:
        case Op::STLXP:
            return s == t || s == t2 || (s == n && n != 31);
        case Op::LDXP:
        case Op::LDAXP:
            return t == t2;
        default:
            return false;
    }
}

class MemorySemanticsTest : public testing::TestWithParam<Op> {};

// As SemanticsTest, with the address register pointing into a block of random memory, pc inside
// it, and the index register small (negative too for SXTW / SXTX); the block must end up the same
// as Unicorn's.
TEST_P(MemorySemanticsTest, MatchesUnicorn) {
    const Op op = GetParam();
    const biwa::a64::OpInfo& info = biwa::a64::Info(op);
    std::mt19937_64 rng(Seed(op));
    MemoryRig rig(rng);
    const char address_letter = AddressRegister(op);
    const bool has_base = HasField(op, address_letter);
    const bool has_index = HasField(op, 'm');
    Tally tally;
    for (int tries = 0; tally.run < kSamples && tries < kSamples * 50; ++tries) {
        const std::uint32_t word = RandomAccess(op, rng);
        if (biwa::a64::Decode(word) != op || Unpredictable(op, word)) {
            continue;
        }
        CpuState in = biwa::test::RandomState(rng);
        const std::uint64_t base = rig.Base(rng);
        in.sp = base & ~std::uint64_t{0xF};
        in.pc = rig.Code(rng);
        const std::uint32_t n = biwa::a64::Field(op, word, address_letter);
        if (has_base && n != 31) {
            // Exclusive and ordered accesses aligned: Unicorn 2.1 skips the alignment check on
            // LDAR / STLR and on failing store-exclusives (the fixed tests cover the faults).
            in.x[n] = (IsExclusiveOrOrdered(op) || rng() % 4 == 0) ? base & ~std::uint64_t{0xF} : base;
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
        const std::array<std::uint32_t, 1> words{word};
        const UnicornCpu::Outcome reference = rig.Run(in, words);
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        const StepResult result = GuestStep(cpu, word);
        const std::string memory = rig.MemoryDiff();
        ASSERT_EQ(memory, "") << std::format("{:08x} ({})", word, info.name);
        Compare(info, word, result, cpu, reference, tally, in);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_EQ(tally.run, kSamples);
    EXPECT_LT(tally.exceptions, tally.run);
}

// Load-exclusive then store-exclusive (of the given form), sometimes with CLREX in between or the
// store to another address: the store succeeds only in the plain case, in Biwa as in Unicorn.
class ExclusiveSequenceTest : public testing::TestWithParam<Op> {};

TEST_P(ExclusiveSequenceTest, MatchesUnicorn) {
    const Op store = GetParam();
    const bool pair = store == Op::STXP || store == Op::STLXP;
    std::mt19937_64 rng(Seed(store));
    MemoryRig rig(rng);
    int stored = 0;
    int run = 0;
    while (run < kSamples) {
        // Distinct registers: base n, load into t (and t2), status s, store from u (and u2), and m,
        // the base for the other-address case. Sometimes SP as the base, XZR as status or source.
        std::array<std::uint32_t, 31> order{};
        std::iota(order.begin(), order.end(), 0u);
        std::shuffle(order.begin(), order.end(), rng);
        const auto sometimes_31 = [&](std::uint32_t r) { return rng() % 8 == 0 ? 31u : r; };
        const std::uint32_t n = sometimes_31(order[0]);
        const std::uint32_t t = order[1];
        const std::uint32_t t2 = order[2];
        const std::uint32_t s = sometimes_31(order[3]);
        const std::uint32_t u = sometimes_31(order[4]);
        const std::uint32_t u2 = sometimes_31(order[5]);
        const std::uint32_t m = order[6];
        const std::uint32_t size =
            pair ? 2 + static_cast<std::uint32_t>(rng() % 2) : static_cast<std::uint32_t>(rng() % 4);
        const bool acquire = rng() % 2 != 0;
        const Op load = pair ? (acquire ? Op::LDAXP : Op::LDXP) : (acquire ? Op::LDAXR : Op::LDXR);
        const std::uint32_t size_bits = pair ? (size - 2) << 30 : size << 30;
        const std::uint32_t load_word = biwa::a64::Info(load).value | size_bits | (pair ? t2 << 10 : 0) | (n << 5) | t;
        const int variant = static_cast<int>(rng() % 4);  // 0: CLREX between, 1: other address, else plain
        const std::uint32_t store_base = variant == 1 ? m : n;
        const std::uint32_t store_word =
            biwa::a64::Info(store).value | size_bits | (s << 16) | (pair ? u2 << 10 : 0) | (store_base << 5) | u;
        CpuState in = biwa::test::RandomState(rng);
        const std::uint64_t base = rig.Base(rng) & ~std::uint64_t{0xF};
        in.sp = base;
        if (n != 31) {
            in.x[n] = base;
        }
        in.x[m] = base + 16;
        in.pc = rig.Code(rng);
        std::vector<std::uint32_t> words{load_word};
        if (variant == 0) {
            words.push_back(biwa::a64::Info(Op::CLREX).value | static_cast<std::uint32_t>(rng() % 16) << 8);
        }
        words.push_back(store_word);
        const UnicornCpu::Outcome reference = rig.Run(in, words);
        BiwaCpu cpu = biwa::test::ToBiwa(in);
        for (const std::uint32_t word : words) {
            ASSERT_EQ(GuestStep(cpu, word), StepResult::kOk) << std::format("{:08x}", word);
        }
        ASSERT_TRUE(reference.completed) << reference.error;
        ASSERT_EQ(rig.MemoryDiff(), "") << std::format("{:08x} {:08x}", load_word, store_word);
        const std::string diff = biwa::test::Diff(reference.state, biwa::test::FromBiwa(cpu));
        ASSERT_EQ(diff, "") << std::format("{:08x} {:08x}, Unicorn != Biwa:\n", load_word, store_word) << diff;
        if (s != 31) {
            EXPECT_EQ(cpu.x[s] == 0, variant >= 2) << std::format("{:08x} {:08x}", load_word, store_word);
            stored += cpu.x[s] == 0 ? 1 : 0;
        }
        ++run;
    }
    EXPECT_GT(stored, kSamples / 4);
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

// Exclusives, load-acquire and store-release; CLREX.
INSTANTIATE_TEST_SUITE_P(Exclusives, MemorySemanticsTest,
                         testing::Values(Op::LDXR, Op::LDAXR, Op::LDXP, Op::LDAXP, Op::STXR, Op::STLXR, Op::STXP,
                                         Op::STLXP, Op::LDAR, Op::STLR),
                         Name);
INSTANTIATE_TEST_SUITE_P(Exclusives, ExclusiveSequenceTest, testing::Values(Op::STXR, Op::STLXR, Op::STXP, Op::STLXP),
                         Name);

// Branches (in the block, so the targets are mapped) and DC ZVA.
INSTANTIATE_TEST_SUITE_P(Branches, MemorySemanticsTest,
                         testing::Values(Op::B_uncond, Op::BL, Op::B_cond, Op::CBZ, Op::CBNZ, Op::TBZ, Op::TBNZ, Op::BR,
                                         Op::BLR, Op::RET, Op::DC_ZVA),
                         Name);

// System instructions that run at EL0.
INSTANTIATE_TEST_SUITE_P(System, SemanticsTest,
                         testing::Values(Op::HINT, Op::NOP, Op::YIELD, Op::WFE, Op::WFI, Op::SEV, Op::SEVL, Op::CLREX,
                                         Op::DSB, Op::DMB, Op::ISB, Op::MRS, Op::MSR_reg, Op::DC_CVAC, Op::DC_CVAU,
                                         Op::DC_CIVAC, Op::IC_IVAU),
                         Name);

INSTANTIATE_TEST_SUITE_P(System, ExceptionTest,
                         testing::Values(Op::SVC, Op::BRK, Op::LDLAR, Op::STLLR, Op::DC_IVAC, Op::DC_ISW, Op::DC_CSW,
                                         Op::DC_CISW, Op::IC_IALLU, Op::IC_IALLUIS, Op::DC_CVAP, Op::CFINV, Op::RMIF,
                                         Op::XAFlag, Op::AXFlag),
                         Name);

// Floating point: moves, compares and selects.
INSTANTIATE_TEST_SUITE_P(FloatMoves, SemanticsTest,
                         testing::Values(Op::FMOV_float, Op::FABS_float, Op::FNEG_float, Op::FMOV_float_imm,
                                         Op::FMOV_float_gen, Op::FCSEL_float, Op::FCMP_float, Op::FCMPE_float,
                                         Op::FCCMP_float, Op::FCCMPE_float),
                         Name);

// Floating point: arithmetic.
INSTANTIATE_TEST_SUITE_P(FloatArithmetic, SemanticsTest,
                         testing::Values(Op::FADD_float, Op::FSUB_float, Op::FMUL_float, Op::FDIV_float,
                                         Op::FNMUL_float, Op::FSQRT_float, Op::FMAX_float, Op::FMIN_float,
                                         Op::FMAXNM_float, Op::FMINNM_float, Op::FMADD_float, Op::FMSUB_float,
                                         Op::FNMADD_float, Op::FNMSUB_float),
                         Name);

// Floating point: conversions.
INSTANTIATE_TEST_SUITE_P(FloatConversions, SemanticsTest,
                         testing::Values(Op::FRINTN_float, Op::FRINTP_float, Op::FRINTM_float, Op::FRINTZ_float,
                                         Op::FRINTA_float, Op::FRINTX_float, Op::FRINTI_float, Op::FCVT_float,
                                         Op::SCVTF_float_fix, Op::UCVTF_float_fix, Op::FCVTZS_float_fix,
                                         Op::FCVTZU_float_fix, Op::FCVTNS_float, Op::FCVTNU_float, Op::SCVTF_float_int,
                                         Op::UCVTF_float_int, Op::FCVTAS_float, Op::FCVTAU_float, Op::FCVTPS_float,
                                         Op::FCVTPU_float, Op::FCVTMS_float, Op::FCVTMU_float, Op::FCVTZS_float_int,
                                         Op::FCVTZU_float_int),
                         Name);

}  // namespace
