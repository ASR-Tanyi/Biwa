// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>

#include "biwa/a64/cpu.h"

namespace biwa::a64 {

enum class StepResult : std::uint8_t {
    kOk,              // executed; pc moved on
    kUndefined,       // not an instruction (unallocated or reserved encoding): pc unchanged
    kNotImplemented,  // an instruction the interpreter cannot run yet: pc unchanged
};

// Executes `word`, the instruction at cpu.pc, through the shared semantics (biwa/a64/a64_sem.h)
// and advances pc. Reserved field values are rejected here, before any semantics run, the same
// check the lifter makes (docs/design/04-cpu-recompiler.md).
StepResult Step(BiwaCpu& cpu, std::uint32_t word);

}  // namespace biwa::a64
