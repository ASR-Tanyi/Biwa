// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace biwa {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Incremental SHA-256 (Mbed TLS underneath). One instance per thread.
class Sha256 {
   public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Update(std::span<const std::byte> data);
    // Returns the digest and starts over, so the object can hash the next input.
    Sha256Digest Finish();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Sha256Digest Sha256Of(std::span<const std::byte> data);

}  // namespace biwa
