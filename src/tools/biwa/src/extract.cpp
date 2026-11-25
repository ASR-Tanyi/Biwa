// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// `biwa extract`: turns a game's NSPs (the base game, its update, add-on content) into a work
// folder (docs/design/03-extract.md, "The work folder").

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "biwa/cli/run.h"
#include "biwa/core/file.h"
#include "biwa/core/hex.h"
#include "biwa/core/json.h"
#include "biwa/core/sha256.h"
#include "biwa/core/version.h"
#include "biwa/nx/cnmt.h"
#include "biwa/nx/content.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/npdm.h"
#include "biwa/nx/nso.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/storage.h"
#include "commands.h"

namespace biwa::cli {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage = "-k <prod.keys> -o <work> [--no-verify] <nsp>...";

struct OpenedNca {
    std::string name;
    nx::StoragePtr storage;
    nx::NcaHeader header;
};

// One input NSP: its NCAs and its content meta.
struct Package {
    fs::path path;
    std::vector<OpenedNca> ncas;
    nx::Cnmt cnmt;
};

std::string TitleText(std::uint64_t id) { return std::format("{:016X}", id); }

Result<std::vector<std::byte>> ReadAll(const nx::Storage& storage) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(storage.Size()));
    BIWA_RETURN_IF_ERROR(storage.Read(0, bytes));
    return bytes;
}

// Opens an NSP: reads its tickets into `tickets`, every NCA header, and the content meta.
Result<Package> OpenPackage(const fs::path& path, const nx::KeySet& keys, nx::TicketMap& tickets, nx::HashCheck check,
                            std::ostream& err) {
    Result<nx::StoragePtr> storage = nx::OpenFileStorage(path);
    if (!storage) {
        return std::unexpected(std::move(storage).error());
    }
    Result<nx::Pfs0> pfs = nx::Pfs0::Open(std::move(*storage));
    if (!pfs) {
        return std::unexpected(std::move(pfs).error().WithContext(PathText(path)));
    }
    tickets.merge(nx::ReadTickets(*pfs, [&](std::string_view name, const Error& error) {
        err << "biwa extract: warning: " << name << ": " << error.message() << '\n';
    }));

    Package package{path, {}, {}};
    for (const nx::Pfs0Entry& entry : pfs->entries()) {
        if (!entry.name.ends_with(".nca")) {
            continue;
        }
        Result<nx::StoragePtr> nca = pfs->OpenEntry(entry);
        Result<nx::NcaHeader> header =
            nca ? nx::ReadNcaHeader(**nca, keys) : Result<nx::NcaHeader>(std::unexpected(nca.error()));
        if (!header) {
            return std::unexpected(std::move(header).error().WithContext(entry.name));
        }
        package.ncas.push_back({entry.name, std::move(*nca), std::move(*header)});
    }

    const auto meta = std::ranges::find(package.ncas, nx::NcaContentType::kMeta,
                                        [](const OpenedNca& nca) { return nca.header.content_type; });
    if (meta == package.ncas.end() || meta->header.sections.empty()) {
        return Failf(Errc::kInvalidArgument, "{} has no meta NCA", PathText(path));
    }
    Result<nx::StoragePtr> meta_fs =
        nx::OpenSectionFs(meta->storage, meta->header, meta->header.sections[0], keys, tickets, check);
    if (!meta_fs) {
        return std::unexpected(std::move(meta_fs).error().WithContext(meta->name));
    }
    Result<nx::Pfs0> meta_pfs = nx::Pfs0::Open(std::move(*meta_fs));
    if (!meta_pfs) {
        return std::unexpected(std::move(meta_pfs).error().WithContext(meta->name));
    }
    const auto cnmt_entry =
        std::ranges::find_if(meta_pfs->entries(), [](const nx::Pfs0Entry& e) { return e.name.ends_with(".cnmt"); });
    if (cnmt_entry == meta_pfs->entries().end()) {
        return Failf(Errc::kCorrupt, "{}: no .cnmt file", meta->name);
    }
    Result<nx::StoragePtr> cnmt_storage = meta_pfs->OpenEntry(*cnmt_entry);
    Result<std::vector<std::byte>> cnmt_bytes =
        cnmt_storage ? ReadAll(**cnmt_storage) : Result<std::vector<std::byte>>(std::unexpected(cnmt_storage.error()));
    if (!cnmt_bytes) {
        return std::unexpected(std::move(cnmt_bytes).error().WithContext(meta->name));
    }
    Result<nx::Cnmt> cnmt = nx::ParseCnmt(*cnmt_bytes);
    if (!cnmt) {
        return std::unexpected(std::move(cnmt).error().WithContext(meta->name));
    }
    package.cnmt = std::move(*cnmt);
    return package;
}

// The NCA the content meta lists for `type`, or nullptr if it lists none.
Result<const OpenedNca*> FindContent(const Package& package, nx::ContentType type) {
    const auto record = std::ranges::find(package.cnmt.contents, type, &nx::ContentRecord::type);
    if (record == package.cnmt.contents.end()) {
        return nullptr;
    }
    const std::string name = ToHex(record->id) + ".nca";
    const auto nca = std::ranges::find(package.ncas, name, &OpenedNca::name);
    if (nca == package.ncas.end()) {
        return Failf(Errc::kCorrupt, "{} lists {} but does not contain it", PathText(package.path), name);
    }
    return &*nca;
}

// The inputs, sorted out by what they are.
struct Inputs {
    Package base;
    std::optional<Package> update;
    std::vector<Package> dlc;
};

Result<Inputs> SortInputs(std::vector<Package> packages) {
    std::optional<Package> base;
    std::optional<Package> update;
    std::vector<Package> dlc;
    for (Package& package : packages) {
        const std::string file = PathText(package.path.filename());
        switch (package.cnmt.type) {
            case nx::ContentMetaType::kApplication:
                if (base) {
                    return Failf(Errc::kInvalidArgument, "{}: only one base game at a time", file);
                }
                base = std::move(package);
                break;
            case nx::ContentMetaType::kPatch:
                if (update) {
                    return Failf(Errc::kInvalidArgument, "{}: only one update at a time (give the latest)", file);
                }
                update = std::move(package);
                break;
            case nx::ContentMetaType::kAddOnContent:
                dlc.push_back(std::move(package));
                break;
            default:
                return Failf(Errc::kUnsupported, "{}: {} packages are not supported", file,
                             nx::ContentMetaTypeName(package.cnmt.type));
        }
    }
    if (!base) {
        return Fail(Errc::kInvalidArgument, "the base game's NSP is needed");
    }
    const std::uint64_t title = base->cnmt.title_id;
    auto check = [&](const Package& package) -> Status {
        if (package.cnmt.related_title_id != title) {
            return Failf(Errc::kInvalidArgument, "{} is for {}, not for {}", PathText(package.path.filename()),
                         TitleText(package.cnmt.related_title_id), TitleText(title));
        }
        return {};
    };
    if (update) {
        BIWA_RETURN_IF_ERROR(check(*update));
    }
    for (const Package& package : dlc) {
        BIWA_RETURN_IF_ERROR(check(package));
    }
    Inputs inputs{std::move(*base), std::move(update), std::move(dlc)};
    return inputs;
}

Json SegmentJson(const nx::NsoSegment& segment) {
    return Json{{"memory_offset", segment.memory_offset},
                {"size", segment.size},
                {"file_size", segment.file_size},
                {"compressed", segment.compressed}};
}

Json ModuleJson(std::string_view name, const nx::Nso& nso) {
    Json json{{"name", name},
              {"build_id", ToHex(nso.build_id)},
              {"image_size", nso.image.size()},
              {"bss_size", nso.bss_size},
              {"segments",
               {{"text", SegmentJson(nso.segment(nx::NsoSegmentKind::kText))},
                {"rodata", SegmentJson(nso.segment(nx::NsoSegmentKind::kRodata))},
                {"data", SegmentJson(nso.segment(nx::NsoSegmentKind::kData))}}}};
    if (nso.mod0) {
        json["mod0"] = Json{{"offset", nso.mod0->offset},
                            {"dynamic", nso.mod0->dynamic},
                            {"bss_start", nso.mod0->bss_start},
                            {"bss_end", nso.mod0->bss_end},
                            {"eh_frame_hdr_start", nso.mod0->eh_frame_hdr_start},
                            {"eh_frame_hdr_end", nso.mod0->eh_frame_hdr_end},
                            {"module_object", nso.mod0->module_object}};
    } else {
        json["mod0"] = nullptr;
    }
    return json;
}

Json NpdmJson(const nx::Npdm& npdm) {
    const nx::KernelCapabilities& caps = npdm.capabilities;
    Json svcs = Json::array();
    for (std::size_t i = 0; i < caps.svcs.size(); ++i) {
        if (caps.svcs.test(i)) {
            svcs.push_back(i);
        }
    }
    Json json{{"name", npdm.name},
              {"program_id", TitleText(npdm.program_id)},
              {"is_64bit", npdm.is_64bit},
              {"address_space_type", npdm.address_space_type},
              {"main_thread",
               {{"priority", npdm.main_thread_priority},
                {"core", npdm.main_thread_core},
                {"stack_size", npdm.main_thread_stack_size}}},
              {"system_resource_size", npdm.system_resource_size}};
    if (caps.thread_info) {
        json["thread_priorities"] = {{"highest", caps.thread_info->highest_priority},
                                     {"lowest", caps.thread_info->lowest_priority}};
        json["cores"] = {{"min", caps.thread_info->min_core}, {"max", caps.thread_info->max_core}};
    }
    json["svcs"] = std::move(svcs);
    auto optional = [&](std::string_view key, const std::optional<std::uint32_t>& value) {
        json[key] = value ? Json(*value) : Json(nullptr);
    };
    optional("kernel_version", caps.kernel_version);
    optional("handle_table_size", caps.handle_table_size);
    optional("program_type", caps.program_type);
    optional("debug_flags", caps.debug_flags);
    return json;
}

Json InputJson(const Package& package) {
    Json contents = Json::array();
    for (const nx::ContentRecord& record : package.cnmt.contents) {
        contents.push_back(
            {{"type", nx::ContentTypeName(record.type)}, {"id", ToHex(record.id)}, {"size", record.size}});
    }
    return Json{{"file", PathText(package.path.filename())},
                {"type", nx::ContentMetaTypeName(package.cnmt.type)},
                {"title_id", TitleText(package.cnmt.title_id)},
                {"version", package.cnmt.version},
                {"required_system_version", package.cnmt.required_system_version},
                {"contents", std::move(contents)}};
}

// Writes the files of the work folder and records each in the manifest's output list.
class WorkFolder {
   public:
    explicit WorkFolder(fs::path root) : root_(std::move(root)) {}

    Status Write(const std::string& relative, std::span<const std::byte> bytes) {
        const fs::path path = root_ / fs::path(relative);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return Failf(Errc::kIo, "cannot create {}: {}", PathText(path.parent_path()), ec.message());
        }
        BIWA_RETURN_IF_ERROR(WriteFileAtomic(path, bytes));
        outputs_.push_back({{"path", relative}, {"size", bytes.size()}, {"sha256", ToHex(Sha256Of(bytes))}});
        return {};
    }

    Status WriteJsonFile(const std::string& relative, const Json& json) {
        const std::string text = json.dump(2) + "\n";
        return Write(relative, std::as_bytes(std::span(text)));
    }

    [[nodiscard]] const Json& outputs() const { return outputs_; }

   private:
    fs::path root_;
    Json outputs_ = Json::array();
};

// exefs/, modules/ and npdm.json, from the program NCA's ExeFS.
Status WriteExefs(const OpenedNca& program, const nx::KeySet& keys, const nx::TicketMap& tickets, nx::HashCheck check,
                  WorkFolder& work, std::ostream& out) {
    const auto section =
        std::ranges::find(program.header.sections, nx::NcaFsType::kPartitionFs, &nx::NcaSection::fs_type);
    if (section == program.header.sections.end()) {
        return Failf(Errc::kCorrupt, "{} has no ExeFS", program.name);
    }
    Result<nx::StoragePtr> fs = nx::OpenSectionFs(program.storage, program.header, *section, keys, tickets, check);
    if (!fs) {
        return std::unexpected(std::move(fs).error().WithContext(program.name));
    }
    Result<nx::Pfs0> exefs = nx::Pfs0::Open(std::move(*fs));
    if (!exefs) {
        return std::unexpected(std::move(exefs).error().WithContext(program.name + " ExeFS"));
    }
    for (const nx::Pfs0Entry& entry : exefs->entries()) {
        Result<nx::StoragePtr> file = exefs->OpenEntry(entry);
        Result<std::vector<std::byte>> bytes =
            file ? ReadAll(**file) : Result<std::vector<std::byte>>(std::unexpected(file.error()));
        if (!bytes) {
            return std::unexpected(std::move(bytes).error().WithContext("exefs/" + entry.name));
        }
        BIWA_RETURN_IF_ERROR(work.Write("exefs/" + entry.name, *bytes));
        if (entry.name == "main.npdm") {
            Result<nx::Npdm> npdm = nx::ParseNpdm(*bytes);
            if (!npdm) {
                return std::unexpected(std::move(npdm).error().WithContext("exefs/main.npdm"));
            }
            BIWA_RETURN_IF_ERROR(work.WriteJsonFile("npdm.json", NpdmJson(*npdm)));
            continue;
        }
        Result<nx::Nso> nso = nx::ReadNso(*nx::MakeMemoryStorage(std::move(*bytes)));
        if (!nso) {
            if (nso.error().code() == Errc::kInvalidArgument) {
                continue;  // not a module (e.g. the logo files some ExeFS carry)
            }
            return std::unexpected(std::move(nso).error().WithContext("exefs/" + entry.name));
        }
        BIWA_RETURN_IF_ERROR(work.Write("modules/" + entry.name + ".bin", nso->image));
        BIWA_RETURN_IF_ERROR(work.WriteJsonFile("modules/" + entry.name + ".json", ModuleJson(entry.name, *nso)));
        out << std::format("  module {:<10} build ID {}, {} bytes\n", entry.name, ToHex(nso->build_id),
                           nso->image.size());
    }
    return {};
}

Status Extract(const Inputs& inputs, const nx::KeySet& keys, const nx::TicketMap& tickets, nx::HashCheck check,
               const fs::path& root, std::ostream& out) {
    WorkFolder work(root);
    const Package& base = inputs.base;
    out << std::format("{} version {}", TitleText(base.cnmt.title_id), base.cnmt.version);
    if (inputs.update) {
        out << std::format(", update version {}", inputs.update->cnmt.version);
    }
    out << '\n';

    // The update's ExeFS replaces the base game's.
    const Package& code = inputs.update ? *inputs.update : base;
    Result<const OpenedNca*> program = FindContent(code, nx::ContentType::kProgram);
    if (!program) {
        return std::unexpected(program.error());
    }
    if (*program == nullptr) {
        return Failf(Errc::kCorrupt, "{} has no program NCA", PathText(code.path.filename()));
    }
    BIWA_RETURN_IF_ERROR(WriteExefs(**program, keys, tickets, check, work, out));

    Json manifest{{"format", 1},
                  {"biwa", std::format("{} ({})", Version(), SourceCommit())},
                  {"title_id", TitleText(base.cnmt.title_id)},
                  {"version", base.cnmt.version}};
    manifest["update"] = inputs.update ? Json{{"title_id", TitleText(inputs.update->cnmt.title_id)},
                                              {"version", inputs.update->cnmt.version}}
                                       : Json(nullptr);
    Json input_list = Json::array({InputJson(base)});
    if (inputs.update) {
        input_list.push_back(InputJson(*inputs.update));
    }
    for (const Package& dlc : inputs.dlc) {
        input_list.push_back(InputJson(dlc));
    }
    manifest["inputs"] = std::move(input_list);
    manifest["outputs"] = work.outputs();
    BIWA_RETURN_IF_ERROR(WriteJson(root / "manifest.json", manifest));
    out << std::format("wrote {} files to {}\n", work.outputs().size() + 1, PathText(root));
    return {};
}

}  // namespace

int RunExtract(Args args, std::ostream& out, std::ostream& err) {
    std::optional<std::string_view> keys_path;
    std::optional<std::string_view> work;
    nx::HashCheck check = nx::HashCheck::kOn;
    std::vector<std::string_view> files;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-k" || args[i] == "--keys" || args[i] == "-o") {
            if (i + 1 >= args.size()) {
                return UsageError("extract", kUsage, std::format("{} needs a value", args[i]), err);
            }
            if (args[i] == "-o") {
                work = args[i + 1];
            } else {
                keys_path = args[i + 1];
            }
            ++i;
        } else if (args[i] == "--no-verify") {
            check = nx::HashCheck::kOff;
        } else {
            files.push_back(args[i]);
        }
    }
    if (!keys_path || !work || files.empty()) {
        return UsageError("extract", kUsage, "-k, -o and at least one NSP are needed", err);
    }
    Result<nx::KeySet> keys = nx::KeySet::Load(fs::path(*keys_path));
    if (!keys) {
        return Report("extract", keys.error(), err);
    }

    nx::TicketMap tickets;
    std::vector<Package> packages;
    for (const std::string_view file : files) {
        Result<Package> package = OpenPackage(fs::path(file), *keys, tickets, check, err);
        if (!package) {
            return Report("extract", package.error(), err);
        }
        packages.push_back(std::move(*package));
    }
    Result<Inputs> inputs = SortInputs(std::move(packages));
    if (!inputs) {
        return Report("extract", inputs.error(), err);
    }
    const Status done = Extract(*inputs, *keys, tickets, check, fs::path(*work), out);
    return done ? kExitOk : Report("extract", done.error(), err);
}

}  // namespace biwa::cli
