// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/crypto.h"

#include <mbedtls/aes.h>

namespace biwa::nx {
namespace {

unsigned char* Bytes(std::span<std::byte> data) { return reinterpret_cast<unsigned char*>(data.data()); }

// RAII for mbedtls_aes_context.
class AesContext {
   public:
    AesContext() { mbedtls_aes_init(&context_); }
    ~AesContext() { mbedtls_aes_free(&context_); }
    AesContext(const AesContext&) = delete;
    AesContext& operator=(const AesContext&) = delete;
    AesContext(AesContext&&) = delete;
    AesContext& operator=(AesContext&&) = delete;
    mbedtls_aes_context* get() { return &context_; }

   private:
    mbedtls_aes_context context_{};
};

class XtsContext {
   public:
    XtsContext() { mbedtls_aes_xts_init(&context_); }
    ~XtsContext() { mbedtls_aes_xts_free(&context_); }
    XtsContext(const XtsContext&) = delete;
    XtsContext& operator=(const XtsContext&) = delete;
    XtsContext(XtsContext&&) = delete;
    XtsContext& operator=(XtsContext&&) = delete;
    mbedtls_aes_xts_context* get() { return &context_; }

   private:
    mbedtls_aes_xts_context context_{};
};

Status AesEcb(const Aes128Key& key, std::span<std::byte> data, int mode) {
    if (data.size() % 16 != 0) {
        return Failf(Errc::kInternal, "AES-ECB on {} bytes, not a multiple of 16", data.size());
    }
    AesContext aes;
    if (mode == MBEDTLS_AES_DECRYPT) {
        mbedtls_aes_setkey_dec(aes.get(), key.data(), 128);
    } else {
        mbedtls_aes_setkey_enc(aes.get(), key.data(), 128);
    }
    for (std::size_t i = 0; i < data.size(); i += 16) {
        unsigned char* block = Bytes(data.subspan(i, 16));
        mbedtls_aes_crypt_ecb(aes.get(), mode, block, block);
    }
    return {};
}

void PutBigEndian(std::uint8_t* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

Status AesXts(const AesXtsKey& key, std::span<std::byte> data, std::uint64_t first_sector, std::size_t sector_size,
              int mode) {
    if (sector_size == 0 || sector_size % 16 != 0 || data.size() % sector_size != 0) {
        return Failf(Errc::kInternal, "AES-XTS on {} bytes with {}-byte sectors", data.size(), sector_size);
    }
    XtsContext xts;
    if (mode == MBEDTLS_AES_DECRYPT) {
        mbedtls_aes_xts_setkey_dec(xts.get(), key.data(), 256);
    } else {
        mbedtls_aes_xts_setkey_enc(xts.get(), key.data(), 256);
    }
    std::uint64_t sector = first_sector;
    for (std::size_t i = 0; i < data.size(); i += sector_size, ++sector) {
        std::array<std::uint8_t, 16> tweak{};
        PutBigEndian(tweak.data() + 8, sector);
        unsigned char* unit = Bytes(data.subspan(i, sector_size));
        if (mbedtls_aes_crypt_xts(xts.get(), mode, sector_size, tweak.data(), unit, unit) != 0) {
            return Failf(Errc::kInternal, "AES-XTS failed on sector {}", sector);
        }
    }
    return {};
}

}  // namespace

Status AesEcbDecrypt(const Aes128Key& key, std::span<std::byte> data) { return AesEcb(key, data, MBEDTLS_AES_DECRYPT); }

Status AesEcbEncrypt(const Aes128Key& key, std::span<std::byte> data) { return AesEcb(key, data, MBEDTLS_AES_ENCRYPT); }

void AesCtrCrypt(const Aes128Key& key, const AesCounter& counter, std::span<std::byte> data) {
    AesContext aes;
    mbedtls_aes_setkey_enc(aes.get(), key.data(), 128);
    AesCounter nonce = counter;
    std::array<unsigned char, 16> stream{};
    std::size_t offset = 0;
    mbedtls_aes_crypt_ctr(aes.get(), data.size(), &offset, nonce.data(), stream.data(), Bytes(data), Bytes(data));
}

AesCounter NcaCounter(std::uint64_t upper, std::uint64_t offset) {
    AesCounter counter{};
    PutBigEndian(counter.data(), upper);
    PutBigEndian(counter.data() + 8, offset / 16);
    return counter;
}

Status AesXtsDecrypt(const AesXtsKey& key, std::span<std::byte> data, std::uint64_t first_sector,
                     std::size_t sector_size) {
    return AesXts(key, data, first_sector, sector_size, MBEDTLS_AES_DECRYPT);
}

Status AesXtsEncrypt(const AesXtsKey& key, std::span<std::byte> data, std::uint64_t first_sector,
                     std::size_t sector_size) {
    return AesXts(key, data, first_sector, sector_size, MBEDTLS_AES_ENCRYPT);
}

}  // namespace biwa::nx
