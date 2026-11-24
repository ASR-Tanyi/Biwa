// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/sha256.h"

#include <mbedtls/sha256.h>

namespace biwa {

struct Sha256::Impl {
    Impl() {
        mbedtls_sha256_init(&context);
        Start();
    }
    ~Impl() { mbedtls_sha256_free(&context); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // The software implementation cannot fail for SHA-256 (is224 = 0), so return codes are not checked.
    void Start() { mbedtls_sha256_starts(&context, 0); }

    mbedtls_sha256_context context{};
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {}
Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::Update(std::span<const std::byte> data) {
    mbedtls_sha256_update(&impl_->context, reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

Sha256Digest Sha256::Finish() {
    Sha256Digest digest{};
    mbedtls_sha256_finish(&impl_->context, digest.data());
    impl_->Start();
    return digest;
}

Sha256Digest Sha256Of(std::span<const std::byte> data) {
    Sha256 sha;
    sha.Update(data);
    return sha.Finish();
}

}  // namespace biwa
