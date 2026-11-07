#pragma once

// Hızlı toplu kurulum (test ve perf): notları depoya tek tek oluşturmak yerine işlem bütçesine bölünmüş
// toplu işlemlerle yazar. Üretimde karşılığı yoktur; uygulamanın kendi yazma yolları (create, write, import
// dosyası) ayrıca sınanır. Kimlik yoldan türer: iki depoya aynı yol aynı düğüm olarak girer, aynı baytla
// tekrar etkisizdir.
#include "core/note_store.hpp"
#include "core/note_store_impl.hpp"
#include "graph/id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace noteapp::test {

// deleted_at dolu not çöpe gider; çöpün yolu trashed_from'dadır, düğüm kökte durur (canlı ağaçta boş klasör
// doğmaz).
struct ImportedNote {
    core::NotePath path;
    std::string bytes;
    std::optional<std::chrono::system_clock::time_point> updated_at = std::nullopt;
    std::optional<std::chrono::system_clock::time_point> created_at = std::nullopt;
    std::optional<std::chrono::system_clock::time_point> deleted_at = std::nullopt;
};

struct ImportOutcome {
    std::size_t imported = 0;
    std::size_t unchanged = 0;  // aynı yolda aynı baytlar zaten var
};

namespace detail {

inline constexpr std::string_view kImportedOrigin = "unknown";

inline std::int64_t to_ms(std::chrono::system_clock::time_point when) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
}

inline graph::NodeId import_id(const ImportedNote& note) {
    const std::string key = (note.deleted_at ? "trash/" : "live/") + note.path.string();
    return graph::derive_node_id("imported-note", std::as_bytes(std::span(key)));
}

// WriteContext::init_note'un kamu API'siyle karşılığı: düğüm, özellikler, yer ve etiketler. Bağlantılar
// relink_notes'la kurulur (hedef, kaynaktan sonra gelmiş olabilir).
inline std::expected<void, core::Error> put_note(core::WriteContext& context, const graph::NodeId& id,
                                                 const core::NotePath& path, const ImportedNote& note) {
    const std::string text = path.string();
    const std::int64_t updated = to_ms(note.updated_at.value_or(std::chrono::system_clock::now()));
    const std::int64_t created = note.created_at ? to_ms(*note.created_at) : updated;
    NOTEAPP_TRY_GRAPH(context.txn.put_node(id), text);
    NOTEAPP_TRY_GRAPH(context.txn.add_label(id, core::kNoteLabel), text);
    NOTEAPP_TRY_GRAPH(context.txn.set_property(id, core::kBody, graph::Value{graph::Bytes{note.bytes}}),
                      text);
    NOTEAPP_TRY_GRAPH(context.txn.set_property(id, core::kCreatedAt, graph::Value{graph::Timestamp{created}}),
                      text);
    NOTEAPP_TRY_GRAPH(context.txn.set_property(id, core::kUpdatedAt, graph::Value{graph::Timestamp{updated}}),
                      text);
    NOTEAPP_TRY_GRAPH(
        context.txn.set_property(id, core::kNoteOrigin, graph::Value{std::string(kImportedOrigin)}), text);
    NOTEAPP_TRY(context.place_note(id, path));
    return context.update_tags(id, text, note.bytes);
}

inline std::expected<void, core::Error> import_one(core::WriteContext& context, const ImportedNote& note,
                                                   ImportOutcome& outcome) {
    const std::string path = note.path.string();
    const graph::NodeId id = import_id(note);
    auto exists = context.txn.has_node(id);
    if (!exists)
        return std::unexpected(core::from_graph(exists.error(), path));
    if (*exists) {
        auto body = context.txn.property(id, core::kBody);
        if (!body)
            return std::unexpected(core::from_graph(body.error(), path));
        const graph::Bytes* current = std::get_if<graph::Bytes>(&*body);
        if (current == nullptr || current->data != note.bytes)  // kurulum hatası: sessizce ezilmez
            return std::unexpected(
                core::Error{core::Error::Code::AlreadyExists, "error.already_exists", {{"path", path}}});
        ++outcome.unchanged;
        return {};
    }
    if (note.deleted_at) {
        auto leaf = core::NotePath::parse(path.substr(path.rfind('/') + 1));
        if (!leaf)
            return std::unexpected(leaf.error());
        NOTEAPP_TRY(put_note(context, id, *leaf, note));
        NOTEAPP_TRY(context.mark_trashed(id, path, *note.deleted_at));
    } else {
        NOTEAPP_TRY(put_note(context, id, note.path, note));
    }
    ++outcome.imported;
    return {};
}

}  // namespace detail

// İşlem bütçesine bölünür: her işlem atomiktir. İçe aktarılan sürüm en düşük önceliklidir
// (WriteTxn::set_lowest_priority): başka cihazın gerçek düzenlemesini HLC'yle geçemez.
inline std::expected<ImportOutcome, core::Error> import_notes(const core::NoteStore& store,
                                                              std::span<const ImportedNote> notes) {
    ImportOutcome total;
    std::size_t at = 0;
    while (at < notes.size()) {
        std::size_t consumed = 0;
        auto outcome = store.impl().write<ImportOutcome>(
            [&](core::WriteContext& context) -> std::expected<ImportOutcome, core::Error> {
                NOTEAPP_TRY_GRAPH(context.txn.set_lowest_priority(), "import");
                ImportOutcome step;
                for (consumed = 0; at + consumed < notes.size() && (consumed == 0 || !context.over_budget());
                     ++consumed)
                    NOTEAPP_TRY(detail::import_one(context, notes[at + consumed], step));
                return step;
            });
        if (!outcome)
            return std::unexpected(outcome.error());
        total.imported += outcome->imported;
        total.unchanged += outcome->unchanged;
        at += consumed;
    }
    return total;
}

inline std::expected<void, core::Error> import_folders(const core::NoteStore& store,
                                                       std::span<const std::string> folders) {
    std::size_t at = 0;
    while (at < folders.size()) {
        auto step = store.impl().write<std::size_t>(
            [&](core::WriteContext& context) -> std::expected<std::size_t, core::Error> {
                NOTEAPP_TRY_GRAPH(context.txn.set_lowest_priority(), "import");
                std::size_t next = at;
                for (; next < folders.size() && (next == at || !context.over_budget()); ++next) {
                    if (context.snapshot.live_by_path.contains(folders[next]))
                        continue;  // aynı adlı not var: boş klasörü kurmak yol çakışması olurdu
                    if (auto made = context.ensure_folder(folders[next]); !made)
                        return std::unexpected(made.error());
                }
                return next;
            });
        if (!step)
            return std::unexpected(step.error());
        at = *step;
    }
    return {};
}

// Tüm notların [[bağlantı]] kenarlarını yeniden kurar: toplu kurulumda hedef, kaynaktan sonra gelmiş
// olabilir.
inline std::expected<void, core::Error> relink_notes(const core::NoteStore& store) {
    core::NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    return impl.rederive_locked(core::NoteStore::Impl::Derive::Links);
}

}  // namespace noteapp::test
