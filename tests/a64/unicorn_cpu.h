// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "a64/cpu_state.h"

struct uc_struct;

namespace biwa::test {

// Unicorn as the reference A64 CPU for tests (a Cortex-A57 model, like the console's).
// Memory: code at kCodeAddress, data at kDataAddress (kDataSize bytes, read/write).
class UnicornCpu {
   public:
    static constexpr std::uint64_t kCodeAddress = 0x10000;
    static constexpr std::uint64_t kCodeSize = 0x10000;
    static constexpr std::uint64_t kDataAddress = 0x100000;
    static constexpr std::uint64_t kDataSize = 0x10000;

    UnicornCpu();  // throws std::runtime_error if Unicorn cannot start
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

    // Places `code` at kCodeAddress, loads `state` with pc = kCodeAddress, runs until pc passes
    // the last instruction (or the CPU takes an exception), and returns the state then.
    Outcome Run(const CpuState& state, std::span<const std::uint32_t> code);

   private:
    void SetState(const CpuState& state);
    CpuState GetState();

    uc_struct* uc_ = nullptr;
};

}  // namespace biwa::test
