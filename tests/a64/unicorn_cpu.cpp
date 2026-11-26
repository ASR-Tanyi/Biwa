// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "a64/unicorn_cpu.h"

#include <unicorn/unicorn.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace biwa::test {
namespace {

void Check(uc_err err, const char* what) {
    if (err != UC_ERR_OK) {
        throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
    }
}

// X0..X28 are consecutive in Unicorn's register list; X29 and X30 are not.
int XRegister(std::size_t i) {
    if (i == 29) {
        return UC_ARM64_REG_X29;
    }
    if (i == 30) {
        return UC_ARM64_REG_X30;
    }
    return UC_ARM64_REG_X0 + static_cast<int>(i);
}

void WriteSystemRegister(uc_engine* uc, std::uint32_t op0, std::uint32_t op1, std::uint32_t crn, std::uint32_t crm,
                         std::uint32_t op2, std::uint64_t value) {
    uc_arm64_cp_reg reg{.crn = crn, .crm = crm, .op0 = op0, .op1 = op1, .op2 = op2, .val = value};
    Check(uc_reg_write(uc, UC_ARM64_REG_CP_REG, &reg), "write system register");
}

std::uint64_t ReadSystemRegister(uc_engine* uc, std::uint32_t op0, std::uint32_t op1, std::uint32_t crn,
                                 std::uint32_t crm, std::uint32_t op2) {
    uc_arm64_cp_reg reg{.crn = crn, .crm = crm, .op0 = op0, .op1 = op1, .op2 = op2, .val = 0};
    Check(uc_reg_read(uc, UC_ARM64_REG_CP_REG, &reg), "read system register");
    return reg.val;
}

struct CodeRange {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t left;  // instructions still to run: a branch back into the code stops too
};

// Called before each instruction; stopping here keeps the instruction from running.
void StopOutside(uc_engine* uc, std::uint64_t address, std::uint32_t /*size*/, void* user) {
    auto* range = static_cast<CodeRange*>(user);
    if (address < range->begin || address >= range->end || range->left == 0) {
        uc_emu_stop(uc);
        return;
    }
    --range->left;
}

// SCTLR_EL1 bits Horizon sets for EL0: CTR_EL0 reads (UCT), DC ZVA (DZE), cache maintenance (UCI),
// WFE and WFI without trapping (nTWE, nTWI).
constexpr std::uint64_t kSctlrEl0Bits = (1u << 15) | (1u << 14) | (1u << 26) | (1u << 18) | (1u << 16);

}  // namespace

UnicornCpu::UnicornCpu(std::uint64_t data_address, std::uint64_t data_size)
    : data_address_(data_address), data_size_(data_size) {
    Check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc_), "uc_open");
    try {
        Check(uc_ctl_set_cpu_model(uc_, UC_CPU_ARM64_A57), "set CPU model");
        Check(uc_mem_map(uc_, kCodeAddress, kCodeSize, UC_PROT_READ | UC_PROT_EXEC), "map code");
        Check(uc_mem_map(uc_, data_address, data_size, UC_PROT_ALL), "map data");
        // Run as an application under Horizon does: EL0, FP and SIMD enabled (CPACR_EL1.FPEN), the
        // counters readable (CNTKCTL_EL1), 19.2 MHz.
        WriteSystemRegister(uc_, 3, 0, 1, 0, 2, 3u << 20);
        sctlr_ = ReadSystemRegister(uc_, 3, 0, 1, 0, 0) | kSctlrEl0Bits;
        WriteSystemRegister(uc_, 3, 0, 14, 1, 0, 3);
        WriteSystemRegister(uc_, 3, 3, 14, 0, 0, 19200000);
    } catch (...) {
        uc_close(uc_);
        throw;
    }
}

UnicornCpu::~UnicornCpu() {
    if (uc_ != nullptr) {
        uc_close(uc_);
    }
}

void UnicornCpu::WriteData(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    Check(uc_mem_write(uc_, data_address_ + offset, bytes.data(), bytes.size()), "write data");
}

void UnicornCpu::ReadData(std::uint64_t offset, std::span<std::uint8_t> bytes) {
    Check(uc_mem_read(uc_, data_address_ + offset, bytes.data(), bytes.size()), "read data");
}

void UnicornCpu::SetState(const CpuState& state) {
    // Back to EL0 (an exception in the previous run moved the CPU to EL1). Writing PSTATE alone
    // leaves Unicorn's cached translation flags at the old level; a system register write rebuilds
    // them.
    const std::uint32_t pstate = 0;
    Check(uc_reg_write(uc_, UC_ARM64_REG_PSTATE, &pstate), "write pstate");
    WriteSystemRegister(uc_, 3, 0, 1, 0, 0, sctlr_);
    for (std::size_t i = 0; i < state.x.size(); ++i) {
        Check(uc_reg_write(uc_, XRegister(i), &state.x[i]), "write x");
    }
    Check(uc_reg_write(uc_, UC_ARM64_REG_SP, &state.sp), "write sp");
    Check(uc_reg_write(uc_, UC_ARM64_REG_PC, &state.pc), "write pc");
    const std::uint64_t nzcv = state.nzcv;
    Check(uc_reg_write(uc_, UC_ARM64_REG_NZCV, &nzcv), "write nzcv");
    const std::uint64_t fpcr = state.fpcr;
    const std::uint64_t fpsr = state.fpsr;
    Check(uc_reg_write(uc_, UC_ARM64_REG_FPCR, &fpcr), "write fpcr");
    Check(uc_reg_write(uc_, UC_ARM64_REG_FPSR, &fpsr), "write fpsr");
    for (std::size_t i = 0; i < state.v.size(); ++i) {  // Q registers are 16-byte buffers
        Check(uc_reg_write(uc_, UC_ARM64_REG_Q0 + static_cast<int>(i), state.v[i].data()), "write q");
    }
    Check(uc_reg_write(uc_, UC_ARM64_REG_TPIDR_EL0, &state.tpidr_el0), "write tpidr_el0");
    Check(uc_reg_write(uc_, UC_ARM64_REG_TPIDRRO_EL0, &state.tpidrro_el0), "write tpidrro_el0");
}

CpuState UnicornCpu::GetState() {
    CpuState state;
    for (std::size_t i = 0; i < state.x.size(); ++i) {
        Check(uc_reg_read(uc_, XRegister(i), &state.x[i]), "read x");
    }
    Check(uc_reg_read(uc_, UC_ARM64_REG_SP, &state.sp), "read sp");
    Check(uc_reg_read(uc_, UC_ARM64_REG_PC, &state.pc), "read pc");
    std::uint64_t value = 0;
    Check(uc_reg_read(uc_, UC_ARM64_REG_NZCV, &value), "read nzcv");
    state.nzcv = static_cast<std::uint32_t>(value);
    Check(uc_reg_read(uc_, UC_ARM64_REG_FPCR, &value), "read fpcr");
    state.fpcr = static_cast<std::uint32_t>(value);
    Check(uc_reg_read(uc_, UC_ARM64_REG_FPSR, &value), "read fpsr");
    state.fpsr = static_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < state.v.size(); ++i) {
        Check(uc_reg_read(uc_, UC_ARM64_REG_Q0 + static_cast<int>(i), state.v[i].data()), "read q");
    }
    Check(uc_reg_read(uc_, UC_ARM64_REG_TPIDR_EL0, &state.tpidr_el0), "read tpidr_el0");
    Check(uc_reg_read(uc_, UC_ARM64_REG_TPIDRRO_EL0, &state.tpidrro_el0), "read tpidrro_el0");
    return state;
}

UnicornCpu::Outcome UnicornCpu::Run(const CpuState& state, std::span<const std::uint32_t> code) {
    const std::uint64_t bytes_size = code.size() * 4;
    const bool in_data = state.pc >= data_address_ && state.pc - data_address_ + bytes_size <= data_size_;
    if (!in_data && bytes_size > kCodeSize) {
        throw std::runtime_error("too much code");
    }
    const std::uint64_t start_pc = in_data ? state.pc : kCodeAddress;
    std::vector<std::uint8_t> bytes(bytes_size);
    std::memcpy(bytes.data(), code.data(), bytes.size());
    Check(uc_mem_write(uc_, start_pc, bytes.data(), bytes.size()), "write code");
    CpuState start = state;
    start.pc = start_pc;
    SetState(start);
    // A branch out of the code stops at its target, before the instruction there runs (the target
    // must be mapped); running off the end stops at the until address.
    CodeRange range{.begin = start_pc, .end = start_pc + bytes.size(), .left = code.size()};
    uc_hook hook = 0;
    Check(uc_hook_add(uc_, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(&StopOutside), &range, 1, 0), "add hook");
    const uc_err err = uc_emu_start(uc_, start_pc, range.end, 0, 0);
    Check(uc_hook_del(uc_, hook), "delete hook");
    Outcome outcome;
    outcome.state = GetState();
    outcome.completed = err == UC_ERR_OK;
    if (!outcome.completed) {
        outcome.error = uc_strerror(err);
    }
    return outcome;
}

}  // namespace biwa::test
