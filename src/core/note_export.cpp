#include "core/note_export.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <optional>
#include <system_error>

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTrashDir = ".trash";

// Yol üstünde bağ ya da dosya varsa içinden geçilmez.
bool parents_are_safe(const fs::path& dir, const fs::path& relative) {
    fs::path current = dir;
    for (const fs::path& component : relative) {
        current /= component;
        const auto stat = stat_file(current);
        if (!stat)
            return true;  // yok: oluşturulacak
        if (!stat->directory)
            return false;
    }
    return true;
}

// Notlar sırdır: yalnız yeni açılan dizin daralır, kullanıcının var olan dizininin izni değişmez.
std::error_code make_export_directories(const fs::path& dir) {
    std::error_code ec;
    const fs::file_status status = fs::status(dir, ec);
    if (fs::exists(status))
        return fs::is_directory(status) ? std::error_code{}
                                        : std::make_error_code(std::errc::not_a_directory);
    return create_private_directories(dir);
}

// Boş klasörler de düz ağaçta durur.
void export_folders(const NoteStore& store, const fs::path& dir, ExportAllReport& report) {
    for (const std::string& folder : store.folders()) {
        auto checked = NotePath::parse(folder);
        if (!checked || !parents_are_safe(dir, checked->value())) {
            report.refused.push_back({folder, !checked ? "invalid_name" : "link"});
            continue;
        }
        if (make_export_directories(dir / checked->value()))
            report.refused.push_back({folder, "unreadable"});
    }
}

// Reddin nedeni; yazıldıysa boş. Yalnız yerel G/Ç hatası expected'ın hatasıdır. relative: dir altındaki yer.
std::expected<std::optional<std::string>, Error>
write_exported(const fs::path& dir, const fs::path& relative,
               const std::expected<NoteContent, Error>& content) {
    if (!parents_are_safe(dir, relative.parent_path()))
        return std::optional<std::string>("link");
    const fs::path target = dir / relative;
    if (make_export_directories(target.parent_path()) || !content)
        return std::optional<std::string>("unreadable");
    std::error_code ec;
    // Dışlayıcı oluşturma: bağ izlenmez, var olan dosyanın üzerine yazılmaz.
    const FileWriteResult written = write_new_file(target, content->text, true);
    if (written.already_exists || (written.ec && fs::exists(target, ec)))
        return std::optional<std::string>(
            "exists");  // Windows'ta yalnız büyük/küçük harf farkı da buraya düşer
    if (written.ec)
        return std::unexpected(io_error(path_utf8(target), written.ec.message()));
    return std::optional<std::string>{};
}

}  // namespace

std::expected<ExportAllReport, Error> export_all_notes(const NoteStore& store, const fs::path& dir) {
    ExportAllReport report;
    if (const std::error_code ec = make_export_directories(dir))
        return std::unexpected(io_error(path_utf8(dir), ec.message()));
    const auto root = stat_file(dir);
    if (!root || !root->directory)  // kök de bağ olamaz: yazma hedefi başka yere kaçar
        return std::unexpected(invalid_path("link", path_utf8(dir)));
    export_folders(store, dir, report);
    auto notes = store.list();
    std::ranges::sort(notes, {}, [](const NoteInfo& info) { return info.path.string(); });
    for (const NoteInfo& info : notes) {
        // Kaynak güvenilmezdir: yol yeniden doğrulanır (ilk savunma snapshot'ta).
        auto refused = NotePath::parse(info.path.string())
                           ? write_exported(dir, info.path.value(), store.read(info.path))
                           : std::optional<std::string>("invalid_name");
        if (!refused)
            return std::unexpected(refused.error());
        if (*refused)
            report.refused.push_back({info.path.string(), **refused});
        else
            report.written.push_back({info.path, info.path.value()});
    }
    auto trash = store.list_trash();
    if (!trash)
        return std::unexpected(trash.error());
    for (const TrashItem& item : *trash) {
        const fs::path relative = fs::path(kTrashDir) / item.path.value();
        auto refused = write_exported(dir, relative, store.read_trash(item.path));
        if (!refused)
            return std::unexpected(refused.error());
        if (*refused)
            report.refused.push_back({std::string(kTrashDir) + "/" + item.path.string(), **refused});
        else
            report.written.push_back({item.path, relative});
    }
    report.unreachable = store.unreachable();
    return report;
}

}  // namespace noteapp::core
