#pragma once

// Notlar artık dosya değil, graph düğümü: testler de depoya depodan girer. Baytlar aynen yazılır. `directory`
// graph.db'nin dizinidir; yardımcılar ayrı bağlantı açar, yani başka bir süreç gibi yazar.
#include "core/note_store.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace noteapp::test {

inline core::NewNote new_note_at(std::string_view path, std::string_view bytes) {
    const auto slash = path.rfind('/');
    const std::string folder =
        slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
    const std::string file =
        slash == std::string_view::npos ? std::string(path) : std::string(path.substr(slash + 1));
    const auto dot = file.rfind('.');
    return core::NewNote{folder, file.substr(0, dot),
                         dot == std::string::npos ? std::string() : file.substr(dot + 1), std::string(bytes)};
}

// "work/plan.md" → klasör, ad, tip. Yol doluysa ya da reddedilirse false. Uzantısız ad (eski dosyalardan
// gelir) create'in değil içe aktarmanın yoludur.
inline bool seed_note(core::NoteStore& store, std::string_view path, std::string_view bytes,
                      std::chrono::system_clock::time_point when = std::chrono::system_clock::now()) {
    const core::NewNote note = new_note_at(path, bytes);
    if (note.type.empty()) {
        auto parsed = core::NotePath::parse(path);
        if (!parsed)
            return false;
        const core::ImportedNote imported{*parsed, std::string(bytes), when};
        return store.import_notes(std::span(&imported, 1)).has_value();
    }
    const auto created = store.create_exact(note, when);
    return created.has_value() && created->path.string() == path;
}

// Varsa üzerine yazar, yoksa oluşturur: "dosyayı değiştir" testlerinin karşılığı.
inline void upsert_note(const std::filesystem::path& directory, std::string_view path, std::string_view bytes,
                        std::chrono::system_clock::time_point when = std::chrono::system_clock::now()) {
    core::NoteStore store(directory);
    auto parsed = core::NotePath::parse(path);
    REQUIRE(parsed.has_value());
    if (const auto current = store.read(*parsed)) {
        const auto written = store.write(*parsed, bytes, current->revision, when);
        REQUIRE(written.has_value());
        return;
    }
    REQUIRE(seed_note(store, path, bytes, when));
}

// Çok notlu kurulum (perf): işlem başına 500 not, geçişin yolu.
inline void bulk_seed(const std::filesystem::path& directory, std::span<const core::ImportedNote> notes) {
    core::NoteStore store(directory);
    constexpr std::size_t kBatch = 500;
    for (std::size_t at = 0; at < notes.size(); at += kBatch) {
        const auto imported = store.import_notes(notes.subspan(at, std::min(kBatch, notes.size() - at)));
        REQUIRE(imported.has_value());
    }
}

inline std::optional<std::string> note_text(const std::filesystem::path& directory, std::string_view path) {
    const core::NoteStore store(directory);
    auto parsed = core::NotePath::parse(path);
    if (!parsed)
        return std::nullopt;
    auto content = store.read(*parsed);
    return content ? std::optional(std::move(content->text)) : std::nullopt;
}

inline std::string note_body(const std::filesystem::path& directory, std::string_view path) {
    return note_text(directory, path).value_or("<missing>");
}

inline bool note_exists(const std::filesystem::path& directory, std::string_view path) {
    return note_text(directory, path).has_value();
}

inline bool note_in_trash(const std::filesystem::path& directory, std::string_view path) {
    const core::NoteStore store(directory);
    const auto items = store.list_trash();
    return items && std::ranges::any_of(
                        *items, [&](const core::TrashItem& item) { return item.path.string() == path; });
}

inline bool folder_exists(const std::filesystem::path& directory, std::string_view path) {
    const core::NoteStore store(directory);
    const auto folders = store.folders();
    return std::ranges::find(folders, path) != folders.end();
}

inline void erase_note(const std::filesystem::path& directory, std::string_view path) {
    core::NoteStore store(directory);
    auto parsed = core::NotePath::parse(path);
    REQUIRE(parsed.has_value());
    REQUIRE(store.remove(*parsed).has_value());
}

inline core::Revision note_revision(const std::filesystem::path& directory, std::string_view path) {
    const core::NoteStore store(directory);
    auto parsed = core::NotePath::parse(path);
    REQUIRE(parsed.has_value());
    const auto content = store.read(*parsed);
    REQUIRE(content.has_value());
    return content->revision;
}

}  // namespace noteapp::test
