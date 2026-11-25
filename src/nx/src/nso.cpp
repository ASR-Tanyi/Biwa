// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/nso.h"

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

namespace biwa::nx {
namespace {

constexpr std::size_t kHeaderSize = 0x100;
constexpr std::array<std::string_view, 3> kNames = {"text", "rodata", "data"};

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::optional<Mod0> ReadMod0(std::span<const std::byte> image) {
    // The second word of .text is the offset of MOD0 from the module's base.
    if (image.size() < 8) {
        return std::nullopt;
    }
    const auto at = Load<std::uint32_t>(image, 4);
    if (!RangeFits(at, 0x1C, image.size()) || std::memcmp(image.data() + at, "MOD0", 4) != 0) {
        return std::nullopt;
    }
    auto field = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(at) + Load<std::int32_t>(image, at + offset));
    };
    return Mod0{at, field(0x04), field(0x08), field(0x0C), field(0x10), field(0x14), field(0x18)};
}

}  // namespace

Result<Nso> ReadNso(const Storage& file) {
    std::array<std::byte, kHeaderSize> header{};
    if (file.Size() < kHeaderSize) {
        return Fail(Errc::kInvalidArgument, "not an NSO: too small");
    }
    BIWA_RETURN_IF_ERROR(file.Read(0, header));
    if (std::memcmp(header.data(), "NSO0", 4) != 0) {
        return Fail(Errc::kInvalidArgument, "not an NSO: bad magic");
    }
    const std::span<const std::byte> h(header);
    const auto flags = Load<std::uint32_t>(h, 0x0C);

    Nso nso;
    std::memcpy(nso.build_id.data(), header.data() + 0x40, nso.build_id.size());
    nso.bss_size = Load<std::uint32_t>(h, 0x3C);
    std::uint64_t image_size = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        NsoSegment& segment = nso.segments[i];
        segment.file_offset = Load<std::uint32_t>(h, 0x10 + i * 0x10);
        segment.memory_offset = Load<std::uint32_t>(h, 0x14 + i * 0x10);
        segment.size = Load<std::uint32_t>(h, 0x18 + i * 0x10);
        segment.compressed = (flags & (1u << i)) != 0;
        segment.hashed = (flags & (1u << (i + 3))) != 0;
        segment.file_size = segment.compressed ? Load<std::uint32_t>(h, 0x60 + i * 4) : segment.size;
        std::memcpy(segment.hash.data(), header.data() + 0xA0 + i * 0x20, segment.hash.size());

        const std::uint64_t previous_end =
            i == 0 ? 0 : std::uint64_t{nso.segments[i - 1].memory_offset} + nso.segments[i - 1].size;
        if ((i == 0 && segment.memory_offset != 0) || segment.memory_offset < previous_end ||
            !RangeFits(segment.file_offset, segment.file_size, file.Size())) {
            return Failf(Errc::kCorrupt, "NSO: the {} segment does not fit", kNames[i]);
        }
        image_size = std::uint64_t{segment.memory_offset} + segment.size;
    }
    if (image_size > kMaxNsoImageSize) {
        return Failf(Errc::kCorrupt, "NSO: a {}-byte image is too large", image_size);
    }

    nso.image.resize(static_cast<std::size_t>(image_size));
    std::vector<std::byte> stored;
    for (std::size_t i = 0; i < 3; ++i) {
        const NsoSegment& segment = nso.segments[i];
        const std::span<std::byte> target = std::span(nso.image).subspan(segment.memory_offset, segment.size);
        if (segment.compressed) {
            stored.resize(segment.file_size);
            BIWA_RETURN_IF_ERROR(file.Read(segment.file_offset, stored));
            const int got = LZ4_decompress_safe(reinterpret_cast<const char*>(stored.data()),
                                                reinterpret_cast<char*>(target.data()), static_cast<int>(stored.size()),
                                                static_cast<int>(target.size()));
            if (got < 0 || static_cast<std::size_t>(got) != target.size()) {
                return Failf(Errc::kCorrupt, "NSO: the {} segment does not decompress", kNames[i]);
            }
        } else {
            BIWA_RETURN_IF_ERROR(file.Read(segment.file_offset, target));
        }
        if (segment.hashed && Sha256Of(target) != segment.hash) {
            return Failf(Errc::kCorrupt, "NSO: the {} segment does not match its hash", kNames[i]);
        }
    }
    nso.mod0 = ReadMod0(nso.image);
    return nso;
}

}  // namespace biwa::nx
