// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "a64/cpu_state.h"

struct uc_struct;

namespace biwa::test {

// Unicorn as the reference A64 CPU for tests (a Cortex-A57 model, like the console's), running at
// EL0 with the system settings Horizon gives applications.
// Memory: code at kCodeAddress, and one data region (by default kDataSize bytes at kDataAddress)
// that can also hold code.
class UnicornCpu {
   public:
    static constexpr std::uint64_t kCodeAddress = 0x10000;
    static constexpr std::uint64_t kCodeSize = 0x10000;
    static constexpr std::uint64_t kDataAddress = 0x100000;
    static constexpr std::uint64_t kDataSize = 0x10000;

    // Throws std::runtime_error if Unicorn cannot start or map the data region (both page aligned).
    explicit UnicornCpu(std::uint64_t data_address = kDataAddress, std::uint64_t data_size = kDataSize);
    ~UnicornCpu();
    UnicornCpu(const UnicornCpu&) = delete;
    UnicornCpu& operator=(const UnicornCpu&) = delete;

    void WriteData(std::uint64_t offset, std::span<const std::uint8_t> bytes);
    void ReadData(std::uint64_t offset, std::span<std::uint8_t> bytes);

    struct Outcome {
        CpuState state;
        bool completed = false;  // false: the CPU stopped on an exception (undefined, fault, svc)
        std::string error;       // Unicorn's message when it did not complete
    };

    // Places `code` at state.pc when that lies in the data region, otherwise at kCodeAddress (and
    // starts there), runs until pc passes the last instruction, a branch leaves the code (its
    // target must be mapped), or the CPU takes an exception, and returns the state then.
    Outcome Run(const CpuState& state, std::span<const std::uint32_t> code);

   private:
    void SetState(const CpuState& state);
    CpuState GetState();

    uc_struct* uc_ = nullptr;
    std::uint64_t data_address_;
    std::uint64_t data_size_;
    std::uint64_t sctlr_ = 0;
};

}  // namespace biwa::test
