// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "a64/sweep.h"

#include <capstone/capstone.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "biwa/a64/decoder.h"

namespace biwa::test {
namespace {

// The operands with register numbers and immediates left out, so "v0.8h, v1.8h, #3" becomes
// "v.8h, v.8h, #": instructions that differ only in those are counted together.
std::string Shape(std::string_view operands) {
    std::string shape;
    for (std::size_t i = 0; i < operands.size(); ++i) {
        const char c = operands[i];
        if (c == '#') {
            shape += '#';
            while (i + 1 < operands.size() && operands[i + 1] != ',' && operands[i + 1] != ']' &&
                   operands[i + 1] != '}') {
                ++i;
            }
        } else if (std::isdigit(static_cast<unsigned char>(c)) == 0 || (!shape.empty() && shape.back() == '.')) {
            shape += c;
        }
    }
    return shape;
}

// SVE and SME instructions name their own registers (z0, p0, za, zt0); Biwa supports neither.
bool IsSveOrSme(std::string_view operands) {
    for (std::size_t i = 0; i < operands.size(); ++i) {
        const bool start = i == 0 || std::isalnum(static_cast<unsigned char>(operands[i - 1])) == 0;
        if (!start || (operands[i] != 'z' && operands[i] != 'p')) {
            continue;
        }
        const char next = i + 1 < operands.size() ? operands[i + 1] : '\0';
        if (std::isdigit(static_cast<unsigned char>(next)) != 0 ||
            (operands[i] == 'z' && (next == 'a' || next == 't'))) {
            return true;
        }
    }
    return false;
}

// Half-precision arithmetic (ARMv8.2 FullFP16): FP instructions on h registers or .h lanes. The
// ARMv8.0 conversions to and from half precision (fcvt, fcvtl, fcvtn) decode in Biwa.
bool IsHalfPrecision(std::string_view mnemonic, std::string_view shape) {
    const bool fp = mnemonic.starts_with('f') || mnemonic == "scvtf" || mnemonic == "ucvtf";
    if (!fp) {
        return false;
    }
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const bool start = i == 0 || std::isalnum(static_cast<unsigned char>(shape[i - 1])) == 0 || shape[i - 1] == '.';
        const bool end = i + 1 == shape.size() || std::isalnum(static_cast<unsigned char>(shape[i + 1])) == 0;
        if (shape[i] == 'h' && end && (start || std::isdigit(static_cast<unsigned char>(shape[i - 1])) != 0)) {
            return true;
        }
    }
    return false;
}

struct NotSupported {
    std::string_view reason;
    std::vector<std::string_view> mnemonics;
    bool prefix = false;  // `mnemonics` are prefixes
};

// The explicit not-supported list: instructions after ARMv8.0, and ones not available at EL0.
const std::vector<NotSupported>& NotSupportedList() {
    static const std::vector<NotSupported> list = {
        {"RDM (ARMv8.1)", {"sqrdmlah", "sqrdmlsh"}},
        {"pointer authentication (ARMv8.3)",
         {"pacda",  "pacdb",  "pacdza", "pacdzb", "pacga",  "pacia",  "pacib", "paciza", "pacizb", "autda", "autdb",
          "autdza", "autdzb", "autia",  "autib",  "autiza", "autizb", "xpacd", "xpaci",  "braa",   "braaz", "brab",
          "brabz",  "blraa",  "blraaz", "blrab",  "blrabz", "retaa",  "retab", "eretaa", "eretab", "ldraa", "ldrab"}},
        {"MTE (ARMv8.5)",
         {"irg", "gmi", "subp", "subps", "stg", "stzg", "st2g", "stz2g", "ldg", "ldgm", "stgm", "stzgm", "stgp", "dc"}},
        {"dot product, matrix multiply, BFloat16 (ARMv8.2+)",
         {"sdot", "udot", "usdot", "sudot", "smmla", "ummla", "usmmla", "bfdot", "bfmlalb", "bfmlalt", "bfmmla",
          "bfcvt", "bfcvtn", "bfcvtn2"}},
        {"FHM, JSCVT, FRINTTS (ARMv8.2+)",
         {"fmlal", "fmlal2", "fmlsl", "fmlsl2", "fjcvtzs", "frint32x", "frint32z", "frint64x", "frint64z"}},
        {"FlagM, LS64, WFxT, TME, SB, BRBE, XS (ARMv8.4+)",
         {"setf8", "setf16", "rmif", "cfinv", "axflag", "xaflag", "ld64b", "st64b", "st64bv", "st64bv0", "wfet", "wfit",
          "tstart", "tcommit", "ttest", "tcancel", "sb", "brb", "dsb"}},
        {"privileged or debug (not EL0)",
         {"hvc", "smc", "hlt", "dcps1", "dcps2", "dcps3", "eret", "drps", "tlbi", "at", "sys", "sysl", "msr", "mrs",
          "ic"}},
        {"permanently undefined", {"udf"}},
        {"LSE atomics (ARMv8.1)",
         {"cas", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin", "swp", "stadd", "stclr",
          "steor", "stset", "stsmax", "stsmin", "stumax", "stumin"},
         true},
        {"RCpc (ARMv8.3+)", {"ldapr", "ldapur", "stlur"}, true},
        {"SVE and SME",
         {"sqinc", "sqdec", "uqinc", "uqdec", "inc", "dec", "cnt", "addpl", "addvl", "rdvl", "cterm", "smstart",
          "smstop", "setffr", "zero"},
         true},
        {"MOPS (ARMv8.8)", {"cpy", "set"}, true},
        {"BC.cond (ARMv8.8)", {"bc."}, true},
    };
    return list;
}

class Capstone {
   public:
    Capstone() {
        if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle_) != CS_ERR_OK) {
            throw std::runtime_error("cs_open failed");
        }
        insn_ = cs_malloc(handle_);
    }
    ~Capstone() {
        cs_free(insn_, 1);
        cs_close(&handle_);
    }
    Capstone(const Capstone&) = delete;
    Capstone& operator=(const Capstone&) = delete;

    // The decoded instruction, or nullptr.
    const cs_insn* Disassemble(std::uint32_t word) {
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &word, 4);
        const std::uint8_t* code = bytes.data();
        std::size_t size = bytes.size();
        std::uint64_t address = 0x1000;
        return cs_disasm_iter(handle_, &code, &size, &address, insn_) ? insn_ : nullptr;
    }

   private:
    csh handle_ = 0;
    cs_insn* insn_ = nullptr;
};

// Why Biwa does not decode an encoding Capstone decodes, if it is on the not-supported list.
std::optional<std::string_view> Classify(Capstone& capstone, std::uint32_t word, const cs_insn* insn) {
    const std::string mnemonic = insn->mnemonic;
    const std::string operands = insn->op_str;
    if (IsSveOrSme(operands)) {
        return "SVE and SME";
    }
    if (IsHalfPrecision(mnemonic, Shape(operands))) {
        return "half-precision arithmetic (ARMv8.2)";
    }
    for (const NotSupported& entry : NotSupportedList()) {
        const bool listed = std::ranges::any_of(entry.mnemonics, [&](std::string_view m) {
            return entry.prefix ? std::string_view(mnemonic).starts_with(m) : mnemonic == m;
        });
        if (listed) {
            return entry.reason;
        }
    }
    // Capstone ignores bits the architecture says should be one or zero (an unused register field
    // in exclusives, SMULH, ...). If the same instruction with that field as required decodes in
    // Biwa, the encoding is one compilers never emit.
    for (const std::uint32_t field : {0x1Fu << 10, 0x1Fu << 16, 0x1Fu, 0x1Fu << 5}) {
        for (const std::uint32_t value : {field, 0u}) {
            const std::uint32_t fixed = (word & ~field) | value;
            if (fixed == word || !a64::Decode(fixed)) {
                continue;
            }
            const cs_insn* other = capstone.Disassemble(fixed);
            if (other != nullptr && mnemonic == other->mnemonic) {
                return "should-be bits not as required";
            }
        }
    }
    return std::nullopt;
}

void Count(std::map<std::string, std::pair<std::uint64_t, std::uint32_t>>& counts, const std::string& key,
           std::uint32_t word) {
    auto& entry = counts[key];
    if (entry.first++ == 0) {
        entry.second = word;
    }
}

}  // namespace

void SweepResult::Merge(const SweepResult& other) {
    checked += other.checked;
    capstone_decoded += other.capstone_decoded;
    for (const auto& [reason, n] : other.not_supported) {
        not_supported[reason] += n;
    }
    both += other.both;
    biwa_only += other.biwa_only;
    for (const auto& [key, value] : other.missing) {
        auto& entry = missing[key];
        if (entry.first == 0) {
            entry.second = value.second;
        }
        entry.first += value.first;
    }
    for (const auto& [key, value] : other.extra) {
        auto& entry = extra[key];
        if (entry.first == 0) {
            entry.second = value.second;
        }
        entry.first += value.first;
    }
}

SweepResult Sweep(std::uint64_t first, std::uint64_t count, const std::function<std::uint32_t(std::uint64_t)>& word) {
    Capstone capstone;
    SweepResult result;
    for (std::uint64_t i = first; i < first + count; ++i) {
        const std::uint32_t w = word(i);
        ++result.checked;
        const std::optional<a64::Op> op = a64::Decode(w);
        const bool biwa = op.has_value();
        const cs_insn* insn = capstone.Disassemble(w);
        if (insn == nullptr) {
            if (op) {
                ++result.biwa_only;
                Count(result.extra, std::string(a64::Info(*op).id), w);
            }
            continue;
        }
        ++result.capstone_decoded;
        if (biwa) {
            ++result.both;
            continue;
        }
        const std::optional<std::string_view> reason = Classify(capstone, w, insn);
        if (reason) {
            ++result.not_supported[std::string(*reason)];
        } else {
            Count(result.missing, std::string(insn->mnemonic) + " " + Shape(insn->op_str), w);
        }
    }
    return result;
}

}  // namespace biwa::test
