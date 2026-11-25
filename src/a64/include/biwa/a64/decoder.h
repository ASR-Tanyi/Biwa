// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace biwa::a64 {

// One A64 instruction form: an entry of the decoder table (third_party/dynarmic/a64.inc), named
// after it.
enum class Op : std::uint16_t {
#define BIWA_A64_OP(id, name, pattern) id,
#include "biwa/a64/ops.inc"
#undef BIWA_A64_OP
};

namespace detail {
enum OpCounter : std::uint16_t {
#define BIWA_A64_OP(id, name, pattern) kCounter_##id,
#include "biwa/a64/ops.inc"
#undef BIWA_A64_OP
    kOpCounterEnd
};
}  // namespace detail

inline constexpr std::size_t kOpCount = detail::kOpCounterEnd;

struct OpInfo {
    std::string_view id;       // "ADD_imm"
    std::string_view name;     // "ADD (immediate)"
    std::string_view pattern;  // bit 31 first: 0 and 1 must match, '-' is ignored, letters are fields
    std::uint32_t mask = 0;    // the bits that must match ...
    std::uint32_t value = 0;   // ... and their values
    bool unallocated = false;  // a pattern that marks encodings as not allocated
};

const OpInfo& Info(Op op);

// The instruction form `word` encodes; nullopt if it is unallocated or not an instruction Biwa
// handles (extensions after ARMv8.0). More specific patterns win over general ones.
std::optional<Op> Decode(std::uint32_t word);

// The bits of the field named `letter` in `word`, taken from the most significant position of the
// pattern down and concatenated (a field may be split, like ADR's immhi:immlo). 0 if the pattern
// has no such field.
std::uint32_t Field(Op op, std::uint32_t word, char letter);

}  // namespace biwa::a64
