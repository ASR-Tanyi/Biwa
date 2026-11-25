// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "biwa/core/hex.h"
#include "support/meta_builder.h"
#include "support/nca_builder.h"
#include "support/nso_builder.h"
#include "support/pfs0_builder.h"

namespace biwa::test {

// Made-up NSPs for a whole title: a meta NCA with the content meta, and a program NCA. Every NCA
// uses its key area (no tickets): key_area_key_application_00 = TestKey(1), section key TestKey(4).
struct TitleBuild {
    std::uint8_t meta_type = 0x80;  // 0x80 application, 0x81 patch, 0x82 add-on content
    std::uint64_t title_id = 0x0100000000004000;
    std::uint32_t version = 0;
    std::uint64_t related = 0x0100000000004800;
    std::uint8_t id_seed = 0x10;  // makes NCA IDs unique between packages
    std::vector<NcaBuildSection> program_sections;
};

inline NcaBuildOptions TitleNcaOptions(nx::NcaContentType type, std::uint64_t title_id) {
    NcaBuildOptions options;
    options.content_type = type;
    options.program_id = title_id;
    options.key_area_key = TestKey(1);
    options.key_area[2] = TestKey(4);
    return options;
}

// A PFS0 section (HierarchicalSha256, AES-CTR) holding `files`.
inline NcaBuildSection Pfs0Section(const std::vector<BuildFile>& files) {
    const HashedSection hashed = BuildSha256Section(BuildPfs0(files));
    NcaBuildSection section;
    section.fs_type = nx::NcaFsType::kPartitionFs;
    section.hash_type = nx::NcaHashType::kHierarchicalSha256;
    section.encryption = nx::NcaEncryption::kAesCtr;
    section.hash_data = hashed.hash_data;
    section.data = hashed.data;
    return section;
}

// An ExeFS with main (build ID from `main_seed`), rtld and main.npdm.
inline NcaBuildSection ExefsSection(std::uint8_t main_seed, std::uint64_t title_id) {
    NsoBuildOptions main;
    main.segments[0] = TextWithMod0(0x900, 0x2100, 0x2300, 0x2800);
    main.segments[1] = Bytes("main rodata");
    main.segments[2] = std::vector<std::byte>(0x100, std::byte{0x33});
    main.build_id_seed = main_seed;
    NsoBuildOptions rtld = main;
    rtld.segments[1] = Bytes("rtld rodata");
    rtld.build_id_seed = 0x77;
    return Pfs0Section({{"main", BuildNso(main)},
                        {"main.npdm", BuildNpdm({(3u << 24) | (28u << 10) | (59u << 4) | 0x7}, title_id)},
                        {"rtld", BuildNso(rtld)}});
}

inline std::vector<std::byte> BuildTitleNsp(const TitleBuild& title) {
    std::vector<BuildFile> files;
    std::vector<CnmtBuildContent> contents;
    if (!title.program_sections.empty()) {
        CnmtBuildContent program;
        program.id[0] = title.id_seed;
        program.type = 1;
        const std::vector<std::byte> nca =
            BuildNca(TitleNcaOptions(nx::NcaContentType::kProgram, title.title_id), title.program_sections);
        program.size = nca.size();
        contents.push_back(program);
        files.push_back({ToHex(program.id) + ".nca", nca});
    }
    std::array<std::uint8_t, 0x10> meta_id{};
    meta_id[0] = static_cast<std::uint8_t>(title.id_seed + 1);
    const std::vector<std::byte> cnmt =
        BuildCnmt(title.title_id, title.version, title.meta_type, title.related, contents);
    const std::string kind = title.meta_type == 0x80   ? "Application"
                             : title.meta_type == 0x81 ? "Patch"
                                                       : "AddOnContent";
    files.push_back({ToHex(meta_id) + ".cnmt.nca",
                     BuildNca(TitleNcaOptions(nx::NcaContentType::kMeta, title.title_id),
                              {Pfs0Section({{std::format("{}_{:016x}.cnmt", kind, title.title_id), cnmt}})})});
    return BuildPfs0(files);
}

}  // namespace biwa::test
