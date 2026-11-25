// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/npdm.h"

#include <bit>
#include <cstring>
#include <string_view>

#include "biwa/nx/storage.h"

namespace biwa::nx {
namespace {

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::string Text(std::span<const std::byte> bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return std::string(text.substr(0, text.find('\0')));
}

// A part of the file named by an {offset, size} pair at `at`, its offset counted from `base`.
Result<std::span<const std::byte>> Part(std::span<const std::byte> bytes, std::size_t at, std::size_t base,
                                        std::size_t min_size, std::string_view what) {
    const std::uint64_t offset = base + std::uint64_t{Load<std::uint32_t>(bytes, at)};
    const std::uint32_t size = Load<std::uint32_t>(bytes, at + 4);
    if (size < min_size || !RangeFits(offset, size, bytes.size())) {
        return Failf(Errc::kCorrupt, "NPDM: the {} does not fit", what);
    }
    return bytes.subspan(static_cast<std::size_t>(offset), size);
}

}  // namespace

KernelCapabilities DecodeKernelCapabilities(std::span<const std::uint32_t> descriptors) {
    KernelCapabilities caps;
    caps.descriptors.assign(descriptors.begin(), descriptors.end());
    for (const std::uint32_t d : descriptors) {
        // The number of low one bits says what a descriptor is.
        switch (std::countr_one(d)) {
            case 3:
                caps.thread_info = KernelCapabilities::ThreadInfo{
                    static_cast<std::uint8_t>((d >> 4) & 0x3F), static_cast<std::uint8_t>((d >> 10) & 0x3F),
                    static_cast<std::uint8_t>((d >> 16) & 0xFF), static_cast<std::uint8_t>((d >> 24) & 0xFF)};
                break;
            case 4: {
                const std::uint32_t group = d >> 29;
                for (std::uint32_t bit = 0; bit < 24; ++bit) {
                    if ((d >> (5 + bit)) & 1) {
                        caps.svcs.set(group * 24 + bit);
                    }
                }
                break;
            }
            case 13:
                caps.program_type = (d >> 14) & 0x7;
                break;
            case 14:
                caps.kernel_version = d >> 15;
                break;
            case 15:
                caps.handle_table_size = (d >> 16) & 0x3FF;
                break;
            case 16:
                caps.debug_flags = (d >> 17) & 0x7;
                break;
            default:  // memory maps, interrupts, padding: kept in `descriptors`
                break;
        }
    }
    return caps;
}

Result<Npdm> ParseNpdm(std::span<const std::byte> bytes) {
    if (bytes.size() < 0x80 || std::memcmp(bytes.data(), "META", 4) != 0) {
        return Fail(Errc::kInvalidArgument, "not an NPDM");
    }
    Npdm npdm;
    const auto flags = Load<std::uint8_t>(bytes, 0x0C);
    npdm.is_64bit = (flags & 1) != 0;
    npdm.address_space_type = static_cast<std::uint8_t>((flags >> 1) & 0x7);
    npdm.main_thread_priority = Load<std::uint8_t>(bytes, 0x0E);
    npdm.main_thread_core = Load<std::uint8_t>(bytes, 0x0F);
    npdm.system_resource_size = Load<std::uint32_t>(bytes, 0x14);
    npdm.version = Load<std::uint32_t>(bytes, 0x18);
    npdm.main_thread_stack_size = Load<std::uint32_t>(bytes, 0x1C);
    npdm.name = Text(bytes.subspan(0x20, 0x10));
    npdm.product_code = Text(bytes.subspan(0x30, 0x10));

    const Result<std::span<const std::byte>> aci = Part(bytes, 0x70, 0, 0x40, "ACI0");
    if (!aci) {
        return std::unexpected(aci.error());
    }
    if (std::memcmp(aci->data(), "ACI0", 4) != 0) {
        return Fail(Errc::kCorrupt, "NPDM: bad ACI0 magic");
    }
    npdm.program_id = Load<std::uint64_t>(*aci, 0x10);
    const Result<std::span<const std::byte>> kc = Part(*aci, 0x30, 0, 0, "ACI0 kernel capabilities");
    if (!kc) {
        return std::unexpected(kc.error());
    }
    std::vector<std::uint32_t> descriptors(kc->size() / 4);
    if (!descriptors.empty()) {
        std::memcpy(descriptors.data(), kc->data(), descriptors.size() * 4);
    }
    npdm.capabilities = DecodeKernelCapabilities(descriptors);

    const Result<std::span<const std::byte>> acid = Part(bytes, 0x78, 0, 0x240, "ACID");
    if (!acid) {
        return std::unexpected(acid.error());
    }
    if (std::memcmp(acid->data() + 0x200, "ACID", 4) != 0) {
        return Fail(Errc::kCorrupt, "NPDM: bad ACID magic");
    }
    npdm.program_id_min = Load<std::uint64_t>(*acid, 0x210);
    npdm.program_id_max = Load<std::uint64_t>(*acid, 0x218);
    return npdm;
}

}  // namespace biwa::nx
