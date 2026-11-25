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

}  // namespace

UnicornCpu::UnicornCpu() {
    Check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc_), "uc_open");
    Check(uc_ctl_set_cpu_model(uc_, UC_CPU_ARM64_A57), "set CPU model");
    Check(uc_mem_map(uc_, kCodeAddress, kCodeSize, UC_PROT_READ | UC_PROT_EXEC), "map code");
    Check(uc_mem_map(uc_, kDataAddress, kDataSize, UC_PROT_READ | UC_PROT_WRITE), "map data");
}

UnicornCpu::~UnicornCpu() {
    if (uc_ != nullptr) {
        uc_close(uc_);
    }
}

void UnicornCpu::WriteData(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    Check(uc_mem_write(uc_, kDataAddress + offset, bytes.data(), bytes.size()), "write data");
}

void UnicornCpu::ReadData(std::uint64_t offset, std::span<std::uint8_t> bytes) {
    Check(uc_mem_read(uc_, kDataAddress + offset, bytes.data(), bytes.size()), "read data");
}

void UnicornCpu::SetState(const CpuState& state) {
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
    if (code.size() * 4 > kCodeSize) {
        throw std::runtime_error("too much code");
    }
    std::vector<std::uint8_t> bytes(code.size() * 4);
    std::memcpy(bytes.data(), code.data(), bytes.size());
    Check(uc_mem_write(uc_, kCodeAddress, bytes.data(), bytes.size()), "write code");
    CpuState start = state;
    start.pc = kCodeAddress;
    SetState(start);
    const uc_err err = uc_emu_start(uc_, kCodeAddress, kCodeAddress + bytes.size(), 0, 0);
    Outcome outcome;
    outcome.state = GetState();
    outcome.completed = err == UC_ERR_OK;
    if (!outcome.completed) {
        outcome.error = uc_strerror(err);
    }
    return outcome;
}

}  // namespace biwa::test
