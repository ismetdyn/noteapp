#include "app/import_files.hpp"

#include "core/file_stat.hpp"
#include "core/folder_tree.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <expected>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace noteapp::app {

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

core::Error io_error(const std::string& source, const std::string& detail) {
    return {core::Error::Code::Io, "error.io", {{"path", source}, {"detail", detail}}};
}

// Kaynağın adı: `docs/`, `.` ve `..` de adını verir (mutlak yoldan). Kök dizinin adı yoktur: "" döner, içi
// hedefe doğrudan gelir.
std::string base_name(const fs::path& source) {
    std::error_code ec;
    fs::path normal = fs::absolute(source, ec).lexically_normal();
    if (ec)
        normal = source.lexically_normal();
    if (!normal.has_filename())
        normal = normal.parent_path();
    return utf8(normal.filename());
}

std::expected<std::string, std::string> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected("cannot open file");
    std::string bytes(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        return std::unexpected("read failed");
    return bytes;
}

struct Run {
    core::NoteStore& store;
    const ImportOptions& options;
    ImportReport report = {};

    void fail(const std::string& source, core::Error error) {
        report.failed.push_back({source, std::move(error)});
    }
};

void import_entry(Run& run, const fs::path& path, const std::string& folder, const std::string& name,
                  std::string_view given_name);

// Ad seçimi: verilen adın uzantısı, `type`, dosyanın uzantısı, içerik tespiti; ilk dolu olan.
void import_file(Run& run, const fs::path& path, const core::FileStat& stat, const std::string& folder,
                 std::string_view given_name) {
    std::string source = utf8(path);
    auto bytes = read_bytes(path);
    if (!bytes) {
        run.fail(source, io_error(source, bytes.error()));
        return;
    }
    const std::string file_name = utf8(path.filename());
    const core::TypedName from_file = core::split_typed_name(file_name);
    const core::TypedName given = core::split_typed_name(given_name);
    const core::TypedName& chosen = given_name.empty() ? from_file : given;
    std::string type = !given.extension.empty()       ? std::string(given.extension)
                       : !run.options.type.empty()    ? run.options.type
                       : !from_file.extension.empty() ? std::string(from_file.extension)
                                                      : detect::suggest(*bytes, run.options.detect).type;
    const std::string wanted = core::join_folder(folder, std::string(chosen.stem) + "." + type);
    const auto updated = core::to_system_time(stat.mtime);
    const core::NewNote note{.folder = folder,
                             .name = std::string(chosen.stem),
                             .type = std::move(type),
                             .content = std::move(*bytes),
                             .exact_name = run.options.overwrite,
                             .created_at = core::file_created_time(path, stat.mtime)};
    const auto made = run.store.create_exact(note, updated);
    if (made) {
        run.report.wrote = true;
        run.report.imported.push_back({std::move(source), made->path, made->path.string() != wanted});
        return;
    }
    if (!run.options.overwrite || made.error().code != core::Error::Code::AlreadyExists) {
        run.fail(source, made.error());
        return;
    }
    // Ad dolu ve ezmek istendi: diskteki güncel revizyonla yazılır (arada değişen not ModifiedExternally).
    const auto target = core::NotePath::parse(wanted);
    if (!target) {
        run.fail(source, target.error());
        return;
    }
    const auto current = run.store.read(*target);
    if (!current) {
        run.fail(source, current.error());
        return;
    }
    if (const auto written = run.store.write(*target, note.content, current->revision, updated); !written) {
        run.fail(source, written.error());
        return;
    }
    run.report.wrote = true;
    run.report.imported.push_back({std::move(source), *target, false, true});
}

// Klasör önce kurulur: boş alt klasör de yapıda kalır. Girdiler ada göre sıralı (rapor belirlenimli).
void import_directory(Run& run, const fs::path& directory, const std::string& folder) {
    if (!folder.empty()) {
        if (auto made = run.store.create_folder(folder); !made) {
            run.fail(utf8(directory), made.error());
            return;
        }
        run.report.wrote = true;
    }
    std::vector<fs::path> children;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        children.push_back(it->path());
    if (ec)  // okunabilenler yine alınır; eksik kalan sessiz değil
        run.fail(utf8(directory), io_error(utf8(directory), ec.message()));
    std::ranges::sort(children);
    for (const fs::path& child : children) {
        const std::string name = utf8(child.filename());
        if (name.starts_with('.')) {
            ++run.report.hidden;
            continue;
        }
        import_entry(run, child, folder, name, {});
    }
}

// `name`: hedefteki ad (klasörde klasör adı); `given_name` dolu ise kullanıcı verdi, uzantısı tipi seçer.
void import_entry(Run& run, const fs::path& path, const std::string& folder, const std::string& name,
                  std::string_view given_name) {
    const auto stat = core::stat_file(path);
    if (!stat) {
        std::string source = utf8(path);
        if (stat.error() == std::errc::no_such_file_or_directory)
            run.fail(source, {core::Error::Code::NotFound, "error.not_found", {{"path", source}}});
        else
            run.fail(source, io_error(source, stat.error().message()));
        return;
    }
    if (stat->symlink)  // bağ izlenmez: hedefi her yere (döngüye de) gidebilir
        run.report.skipped.push_back({utf8(path), "import_link"});
    else if (stat->directory)
        import_directory(run, path, name.empty() ? folder : core::join_folder(folder, name));
    else if (stat->regular)
        import_file(run, path, *stat, folder, given_name);
    else  // aygıt, boru, soket: okumak bekletebilir ya da bitmeyebilir
        run.report.skipped.push_back({utf8(path), "import_not_file"});
}

}  // namespace

ImportReport import_paths(core::NoteStore& store, std::span<const std::filesystem::path> sources,
                          const ImportOptions& options) {
    Run run{store, options};
    for (const fs::path& source : sources) {
        const std::string name = options.name.empty() ? base_name(source) : options.name;
        import_entry(run, source, options.folder, name, options.name);
    }
    return std::move(run.report);
}

ImportOptions import_options(const core::Config& config, std::string folder) {
    return {.folder = std::move(folder),
            .detect = {config.default_type, config.tuning.min_confidence, config.tuning.max_scan_bytes}};
}

std::vector<ImportLine> import_report_lines(const ImportReport& report) {
    std::vector<ImportLine> lines;
    for (const auto& item : report.imported) {
        const char* key = item.overwritten ? "cli.import_overwritten"
                          : item.renamed   ? "cli.import_renamed"
                                           : "cli.import_done";
        lines.push_back({core::tr(key, {{"source", item.source}, {"path", item.note.string()}})});
    }
    for (const auto& item : report.skipped)
        lines.push_back({core::tr("cli.import_skipped",
                                  {{"source", item.source}, {"reason", core::tr("reason." + item.reason)}}),
                         true});
    for (const auto& item : report.failed)
        lines.push_back({core::tr("cli.import_failed",
                                  {{"source", item.source}, {"error", core::format_error(item.error)}}),
                         true});
    if (report.hidden > 0)
        lines.push_back({core::tr("cli.import_hidden", {{"count", std::to_string(report.hidden)}}), true});
    lines.push_back({core::tr("cli.import_summary", {{"imported", std::to_string(report.imported.size())},
                                                     {"skipped", std::to_string(report.skipped.size())},
                                                     {"failed", std::to_string(report.failed.size())}})});
    return lines;
}

std::string import_status(const ImportReport& report) {
    const std::vector<ImportLine> lines = import_report_lines(report);
    const auto problem = std::ranges::find_if(lines, &ImportLine::problem);
    // Özet son satırdır; sorun yoksa yalnız o.
    return problem == lines.end() ? lines.back().text : lines.back().text + " " + problem->text;
}

}  // namespace noteapp::app
