// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "biwa/core/error.h"

namespace biwa::nx {

inline constexpr std::size_t kSvcCount = 0xC0;  // 8 groups of 24 SVC bits

// The kernel capabilities a program is granted (from ACI0), decoded where Biwa uses them.
struct KernelCapabilities {
    struct ThreadInfo {
        std::uint8_t lowest_priority = 0;   // the largest priority value allowed
        std::uint8_t highest_priority = 0;  // the smallest
        std::uint8_t min_core = 0;
        std::uint8_t max_core = 0;
    };
    std::optional<ThreadInfo> thread_info;
    std::bitset<kSvcCount> svcs;  // allowed SVC numbers
    std::optional<std::uint32_t> kernel_version;
    std::optional<std::uint32_t> handle_table_size;
    std::optional<std::uint32_t> program_type;
    std::optional<std::uint32_t> debug_flags;
    std::vector<std::uint32_t> descriptors;  // all of them, as stored
};

// main.npdm: the META header, ACI0 and ACID (docs/design/03-extract.md, "NSO and NPDM").
struct Npdm {
    std::string name;
    std::string product_code;
    bool is_64bit = false;
    std::uint8_t address_space_type = 0;  // 0: 32-bit, 1: 64-bit (36-bit), 2: 32-bit no reserve, 3: 64-bit (39-bit)
    std::uint8_t main_thread_priority = 0;
    std::uint8_t main_thread_core = 0;
    std::uint32_t main_thread_stack_size = 0;
    std::uint32_t system_resource_size = 0;
    std::uint32_t version = 0;
    std::uint64_t program_id = 0;      // ACI0
    std::uint64_t program_id_min = 0;  // ACID
    std::uint64_t program_id_max = 0;
    KernelCapabilities capabilities;  // ACI0's
};

// Errc::kInvalidArgument if `bytes` is not an NPDM; Errc::kCorrupt for parts that do not fit.
Result<Npdm> ParseNpdm(std::span<const std::byte> bytes);

// Decodes kernel capability descriptors; kinds Biwa does not use are only kept in `descriptors`.
KernelCapabilities DecodeKernelCapabilities(std::span<const std::uint32_t> descriptors);

}  // namespace biwa::nx
