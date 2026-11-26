// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>

#include "biwa/a64/cpu.h"

namespace biwa::a64 {

enum class StepResult : std::uint8_t {
    kOk,              // executed; pc is the next instruction or the branch target
    kUndefined,       // not an instruction at EL0 (unallocated, reserved or privileged): pc unchanged
    kNotImplemented,  // an instruction the interpreter cannot run yet: pc unchanged
    kSupervisorCall,  // SVC: pc is the next instruction; the SVC number is in the word
    kBreakpoint,      // BRK: pc unchanged
    kAlignmentFault,  // an exclusive or ordered access to an unaligned address: pc unchanged
};

// Executes `word`, the instruction at cpu.pc, through the shared semantics (biwa/a64/a64_sem.h)
// and moves pc on. Reserved field values are rejected here, before any semantics run, the same
// check the lifter makes (docs/design/04-cpu-recompiler.md). Instructions run as at EL0 under
// Horizon: cache maintenance, DC ZVA, CTR_EL0 and the wait hints are allowed there.
StepResult Step(BiwaCpu& cpu, std::uint32_t word);

}  // namespace biwa::a64
