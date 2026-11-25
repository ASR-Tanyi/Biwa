// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/keys.h"

#include <format>
#include <utility>

#include "biwa/core/file.h"
#include "biwa/core/hex.h"

namespace biwa::nx {
namespace {

void WipeBytes(std::vector<std::uint8_t>& bytes) {
    volatile std::uint8_t* p = bytes.data();  // volatile: the compiler may not drop the stores
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p[i] = 0;
    }
}

std::string_view Trim(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool IsNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

}  // namespace

KeySet& KeySet::operator=(const KeySet& other) {
    if (this != &other) {
        Wipe();
        keys_ = other.keys_;
    }
    return *this;
}

KeySet& KeySet::operator=(KeySet&& other) noexcept {
    if (this != &other) {
        Wipe();
        keys_ = std::move(other.keys_);
    }
    return *this;
}

KeySet::~KeySet() { Wipe(); }

void KeySet::Wipe() {
    for (auto& [name, bytes] : keys_) {
        WipeBytes(bytes);
    }
    keys_.clear();
}

Result<KeySet> KeySet::Parse(std::string_view text, std::string_view source) {
    KeySet set;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const std::size_t end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return Failf(Errc::kInvalidArgument, "{} line {}: expected 'name = value'", source, line_number);
        }
        const std::string name = Lower(Trim(line.substr(0, equals)));
        if (name.empty() || !std::ranges::all_of(name, IsNameChar)) {
            return Failf(Errc::kInvalidArgument, "{} line {}: bad key name", source, line_number);
        }
        Result<std::vector<std::uint8_t>> value = ParseHex(Trim(line.substr(equals + 1)));
        if (!value || value->empty()) {
            return Failf(Errc::kInvalidArgument, "{} line {}: the value of {} is not hex{}", source, line_number, name,
                         value ? " (empty)" : std::format(" ({})", value.error().message()));
        }
        auto [it, inserted] = set.keys_.try_emplace(name);
        WipeBytes(it->second);
        it->second = std::move(*value);
    }
    return set;
}

Result<KeySet> KeySet::Load(const std::filesystem::path& path) {
    Result<std::string> text = ReadFileText(path);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    const std::u8string name = path.filename().u8string();
    Result<KeySet> set = Parse(*text, std::string(name.begin(), name.end()));
    std::ranges::fill(*text, '\0');
    return set;
}

void KeySet::Merge(const KeySet& other) {
    for (const auto& [name, bytes] : other.keys_) {
        auto [it, inserted] = keys_.try_emplace(name);
        WipeBytes(it->second);
        it->second = bytes;
    }
}

bool KeySet::Contains(std::string_view name) const { return keys_.contains(Lower(name)); }

std::vector<std::string> KeySet::Names() const {
    std::vector<std::string> names;
    names.reserve(keys_.size());
    for (const auto& [name, bytes] : keys_) {
        names.push_back(name);
    }
    return names;
}

Result<std::span<const std::uint8_t>> KeySet::Find(std::string_view name, std::size_t size) const {
    const auto it = keys_.find(Lower(name));
    if (it == keys_.end()) {
        return Failf(Errc::kMissingKey, "{} is not in the key files", Lower(name));
    }
    if (it->second.size() != size) {
        return Failf(Errc::kCorrupt, "{} has {} bytes, expected {}", it->first, it->second.size(), size);
    }
    return std::span<const std::uint8_t>(it->second);
}

std::string IndexedKeyName(std::string_view base, unsigned index) { return std::format("{}_{:02x}", base, index); }

}  // namespace biwa::nx
