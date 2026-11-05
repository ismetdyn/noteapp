#include "app/import_files.hpp"

#include "core/file_stat.hpp"
#include "core/folder_tree.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
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

core::Error out_of_memory(const std::string& source) {
    return {core::Error::Code::Io, "error.out_of_memory", {{"path", source}}};
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

bool is_hidden(const fs::path& entry) {
    return entry.filename().u8string().starts_with(u8'.');
}

// import_entry'nin ele alacağı girdiler: kendisi ve klasörse gizli olmayan içi (bağ izlenmez). Okunamayan
// klasör yalnız kendisidir; içe alma onu zaten hata yapar. Durdurulunca gezinti kısa keser: büyük ya da yavaş
// ağaç durdurmayı ve kapanışı bekletmesin (sayı eksik kalır, içe alma zaten başlamaz).
std::size_t count_entries(const fs::path& path, const std::stop_token& stop) {
    const auto stat = core::stat_file(path);
    if (!stat || stat->symlink || !stat->directory)
        return 1;
    std::size_t count = 1;
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end && !stop.stop_requested();
         it.increment(ec))
        if (!is_hidden(it->path()))
            count += count_entries(it->path(), stop);
    return count;
}

struct Run {
    core::NoteStore& store;
    const ImportOptions& options;
    const ImportControl& control;
    ImportReport report = {};
    std::size_t started = 0;
    std::size_t total = 0;
    bool aborted = false;  // beklenmeyen istisna: depo tutarlı kalsın diye kalan girdiler alınmaz

    void fail(const std::string& source, core::Error error) {
        report.failed.push_back({source, std::move(error)});
    }
};

void import_entry(Run& run, const fs::path& path, const std::string& folder, const std::string& name,
                  std::string_view given_name);
void dispatch_entry(Run& run, const fs::path& path, const std::string& folder, const std::string& name,
                    std::string_view given_name);

// Ad seçimi: verilen adın uzantısı, `type`, dosyanın uzantısı, içerik tespiti; ilk dolu olan.
void import_file(Run& run, const fs::path& path, const core::FileStat& stat, const std::string& folder,
                 std::string_view given_name) {
    std::string source = utf8(path);
    std::expected<std::string, std::string> bytes;
    try {
        bytes = run.control.read ? run.control.read(path) : read_bytes(path);
    } catch (const std::bad_alloc&) {
        // Belleğe sığmayan dosya süreci düşürmesin: yarım okuma bırakılır, kalanlar alınır.
        run.fail(source, out_of_memory(source));
        return;
    } catch (const std::length_error&) {  // string'in en büyük boyutunu aştı: aynı durum
        run.fail(source, out_of_memory(source));
        return;
    }
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
    // Girdi sınırında: yarım not yazılmaz. Klasörün içine inilmez (bir girdi sayılır): durdurma beklemesin.
    if (run.aborted || run.control.stop.stop_requested()) {
        ++run.report.cancelled;
        return;
    }
    if (run.control.progress) {
        ++run.started;
        run.total = std::max(run.total, run.started);  // ön sayımdan sonra eklenen girdi
        run.control.progress({run.started, run.total, utf8(path)});
    }
    // Beklenmeyen istisna (depo, dosya sistemi) süreci düşürmez ve o ana dek alınanları raporsuz bırakmaz.
    // Sürdürülmez: yarıda kalan yazmadan sonra deponun bellekteki görüntüsüne güvenilmez.
    try {
        dispatch_entry(run, path, folder, name, given_name);
    } catch (const std::exception& error) {
        const std::string source = utf8(path);
        core::log_warn(std::string("event=import_failed detail=") + error.what());
        run.fail(source, io_error(source, error.what()));
        run.aborted = true;
        run.report.wrote = true;  // yarıda kalan iş yazmış olabilir: yeniden tarama ve senkron
    }
}

void dispatch_entry(Run& run, const fs::path& path, const std::string& folder, const std::string& name,
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
                          const ImportOptions& options, const ImportControl& control) {
    Run run{store, options, control};
    if (control.progress)
        for (const fs::path& source : sources)
            run.total += count_entries(source, control.stop);
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
    if (report.cancelled > 0)  // tek satırlık durumda ilk sorun olarak görünsün
        lines.push_back(
            {core::tr("cli.import_cancelled", {{"count", std::to_string(report.cancelled)}}), true});
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

bool ImportJob::start(core::NoteStore store, std::vector<std::filesystem::path> sources,
                      ImportOptions options, OnProgress on_progress, OnDone on_done) {
    if (running_ || !post_)  // post yoksa işçi sonucu iletemez (boş std::function çağrısı süreci düşürürdü)
        return false;
    running_ = true;
    on_progress_ = std::move(on_progress);
    on_done_ = std::move(on_done);
    try {
        // Önceki işçi raporunu göndermişti; atama onu durdurup bekler (bitmiş iş parçacığı, anında).
        worker_ = std::jthread([this, store = std::move(store), sources = std::move(sources),
                                options = std::move(options)](const std::stop_token& stop) mutable {
            work(store, sources, options, stop);
        });
    } catch (const std::exception& error) {  // iş parçacığı açılamadı (std::system_error): iş sürmüyor
        core::log_warn(std::string("event=import_start_failed detail=") + error.what());
        running_ = false;
        return false;
    }
    return true;
}

void ImportJob::work(core::NoteStore& store, const std::vector<std::filesystem::path>& sources,
                     const ImportOptions& options, const std::stop_token& stop) {
    ImportReport report;
    // Öğe istisnalarını import_paths kendisi rapora çevirir (alınanlar kalır). Burası son çare: istisna
    // jthread'den kaçarsa std::terminate olurdu.
    try {
        report = import_paths(store, sources, options,
                              {.stop = stop, .progress = [this](const ImportProgress& p) { publish(p); }});
    } catch (const std::exception& error) {
        core::log_warn(std::string("event=import_failed detail=") + error.what());
        const std::string source = sources.empty() ? std::string() : utf8(sources.front());
        report.failed.push_back({source, io_error(source, error.what())});
        report.wrote = true;  // yarıda kalan iş yazmış olabilir: yeniden tarama ve senkron
    }
    post_([this, report = std::move(report)]() mutable {
        running_ = false;
        if (on_done_)
            on_done_(std::move(report));
    });
}

void ImportJob::publish(const ImportProgress& progress) {
    {
        const std::scoped_lock lock(mutex_);
        const bool pending = latest_.has_value();
        latest_ = progress;
        if (pending)  // UI henüz öncekini almadı: o iş en son değeri okur
            return;
    }
    post_([this] {
        std::optional<ImportProgress> latest;
        {
            const std::scoped_lock lock(mutex_);
            latest = std::exchange(latest_, std::nullopt);
        }
        // Durdurulduysa bekleyen ilerleme "durduruluyor" iletisini ezmesin.
        if (latest && on_progress_ && !worker_.get_stop_token().stop_requested())
            on_progress_(*latest);
    });
}

std::string import_progress_text(const ImportProgress& progress) {
    return core::tr("cli.import_progress_item", {{"done", std::to_string(progress.done)},
                                                 {"total", std::to_string(progress.total)},
                                                 {"name", progress.current}});
}

}  // namespace noteapp::app
