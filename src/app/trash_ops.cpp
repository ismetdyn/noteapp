#include "app/trash_ops.hpp"

#include "core/folder_tree.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace noteapp::app {

void BatchReport::fail(std::string path, core::Error error) {
    ++failed;
    lines.push_back({std::move(path), std::move(error)});
}

void BatchReport::renamed(std::string from, std::string to) {
    lines.push_back({std::move(from), std::nullopt, std::move(to)});
}

namespace {

bool ask(const Prompts& prompts, const Confirm& request) {
    return prompts.confirm && prompts.confirm(request);
}

std::size_t note_count(const Listing& listing, std::string_view folder) {
    const core::FolderNode* node = listing.tree().find(folder);
    return node ? node->note_count : 0;
}

// Kullanıcıya gösterilen not: yol ve sorudan önce listedeki revizyon. Yalnız bu hâl silinir ya da çöpe
// atılır: senkron notu değiştirdiyse ya da aynı yola başka not koyduysa not kalır (SD7 review). Revizyon
// deponun işleminin içinde denetlenir (NoteStore::remove/trash `expected`): okuma ile silme arasına yazma
// giremez (SD11b).
struct ShownNote {
    core::NotePath path;
    std::optional<core::Revision> revision;  // listede yoksa (eski liste) denetlenmez
};

ShownNote shown_note(const Listing& listing, const core::NotePath& path) {
    const core::NoteInfo* info = listing.note_of(path);
    return {path, info ? std::optional(info->revision) : std::nullopt};
}

// Soruda arayüzün kendi yazması (Prompts::written) onaylı hâldir: o notun beklenen revizyonu odur.
void apply_written(std::span<ShownNote> notes, const Prompts& prompts) {
    if (!prompts.written)
        return;
    for (ShownNote& note : notes)
        if (auto own = prompts.written(note.path))
            note.revision = own;
}

std::vector<ShownNote> shown_under(const Listing& listing, std::string_view folder) {
    std::vector<ShownNote> notes;
    for (const std::size_t note : listing.tree().notes_under(folder))
        notes.push_back(shown_note(listing, listing.metas()[note].path));
    return notes;
}

// Deponun ModifiedExternally'si kullanıcıya "onaydan sonra değişti" olarak söylenir.
core::Error changed_since_shown(core::Error error, const char* message_key) {
    if (error.code == core::Error::Code::ModifiedExternally)
        error.message_key = message_key;
    return error;
}

std::expected<void, core::Error> remove_shown(core::NoteStore& store, const ShownNote& note) {
    return store.remove(note.path, note.revision).transform_error([](core::Error error) {
        return changed_since_shown(std::move(error), "error.changed_since_confirm");
    });
}

std::expected<core::TrashItem, core::Error> trash_shown(core::NoteStore& store, const ShownNote& note) {
    return store.trash(note.path, note.revision).transform_error([](core::Error error) {
        return changed_since_shown(std::move(error), "error.trash_changed_since_confirm");
    });
}

// Klasörün gösterilen notları tek tek çöpe, sonra boşalan klasör kaldırılır (depo klasörü not başına çöpe
// atmaz: remove_folder force notları kalıcı siler). Arada gelen ya da değişen not klasörü tutar (NotEmpty,
// değişti); kalanlar sürer. Hiç not atılamadıysa klasörler adıma yazılmaz: geri alma senkronun kaldırdığı
// klasörü geri getirmesin.
struct FolderTrash {
    std::vector<core::TrashItem> notes;
    std::vector<std::string> folders;  // geri almada yeniden kurulacaklar
    std::optional<core::Error> failure;
};

FolderTrash trash_shown_folder(core::NoteStore& store, const Listing& listing,
                               std::span<const ShownNote> shown, const std::string& folder) {
    FolderTrash done;
    for (const ShownNote& note : shown) {
        auto trashed = trash_shown(store, note);
        if (!trashed) {
            done.failure = done.failure ? done.failure : trashed.error();
            continue;  // kalanlar sürer, klasör yerinde kalır
        }
        done.notes.push_back(std::move(*trashed));
    }
    if (!done.failure)
        if (auto removed = store.remove_folder(folder, false); !removed)
            done.failure = removed.error();
    if (!done.failure || !done.notes.empty())
        std::ranges::copy(listing.tree().folders_under(folder), std::back_inserter(done.folders));
    return done;
}

std::size_t note_count(const Listing& listing, std::span<const TrashTarget> items) {
    std::size_t notes = 0;
    for (const TrashTarget& item : items)
        notes += item.is_folder ? note_count(listing, item.path.string()) : 1;
    return notes;
}

}  // namespace

ConfirmText confirm_text(const Confirm& request) {
    const std::string& path = request.path;
    const std::string notes = core::count_text("tui.count_notes", request.notes);
    const std::string folders = core::count_text("tui.count_subfolders", request.folders);
    const std::string items = core::count_text("tui.count_items", request.items);
    switch (request.kind) {
    case ConfirmKind::TrashFolder:
        return {core::tr("tui.confirm_trash_folder", {{"path", path}, {"notes", notes}, {"folders", folders}}),
                core::tr("tui.folder_trash_title")};
    case ConfirmKind::TrashBatch:
        return {core::tr("tui.confirm_trash_batch", {{"items", items}, {"notes", notes}}),
                core::tr("tui.batch_trash_title")};
    case ConfirmKind::Delete:
        return {core::tr("tui.confirm_delete", {{"path", path}})};
    case ConfirmKind::Purge:
        return {core::tr("tui.confirm_purge", {{"path", path}})};
    case ConfirmKind::DeleteFolder:
        return {core::tr("tui.confirm_delete_folder", {{"path", path}, {"notes", notes}, {"folders", folders}}),
                core::tr("tui.folder_delete_title")};
    case ConfirmKind::DeleteBatch:
        return {core::tr("tui.confirm_delete_batch", {{"items", items}, {"notes", notes}})};
    case ConfirmKind::EmptyTrash:
        return {core::tr("tui.confirm_empty_trash", {{"count", core::count_text("tui.count_notes", request.items)}}),
                core::tr("tui.empty_trash_title")};
    }
    std::unreachable();
}

void TrashOps::push(Step step) {
    steps_.push_back(std::move(step));
    while (steps_.size() > undo_steps_.cap())
        steps_.pop_front();
}

void TrashOps::forget(const core::NotePath& item) {
    for (Step& step : steps_)
        std::erase_if(step.notes, [&](const core::TrashItem& entry) { return entry.path == item; });
    std::erase_if(steps_, [](const Step& step) {
        return step.notes.empty() && step.folder.empty() && step.folders.empty();
    });
}

std::expected<core::TrashItem, core::Error> TrashOps::trash_note(core::NoteStore& store,
                                                                 const core::NotePath& path) {
    auto trashed = store.trash(path);
    if (trashed)
        push({.notes = {*trashed}});
    return trashed;
}

std::optional<FolderTrashed> TrashOps::trash_folder(core::NoteStore& store, const Listing& listing,
                                                    std::string_view folder, const Prompts& prompts) {
    std::vector<ShownNote> shown = shown_under(listing, folder);  // sorudan önce: sorulan notlar
    if (const core::FolderNode* node = listing.tree().find(folder); node && node->note_count > 0) {
        const Confirm request{.kind = ConfirmKind::TrashFolder,
                              .path = std::string(folder),
                              .notes = node->note_count,
                              .folders = node->folder_count};
        if (!ask(prompts, request))
            return std::nullopt;
    }
    apply_written(shown, prompts);
    FolderTrash done = trash_shown_folder(store, listing, shown, std::string(folder));
    const FolderTrashed result{.notes = done.notes.size(), .failure = std::move(done.failure)};
    if (!done.notes.empty() || !result.failure)
        push({.notes = std::move(done.notes),
              .folders = std::move(done.folders),
              .folder = std::string(folder)});
    return result;
}

std::optional<BatchReport> TrashOps::trash_items(core::NoteStore& store, const Listing& listing,
                                                 std::span<const TrashTarget> items, const Prompts& prompts) {
    const bool has_notes_in_folder = std::ranges::any_of(items, [&](const TrashTarget& item) {
        return item.is_folder && note_count(listing, item.path.string()) > 0;
    });
    // Sorudan önce: her öğenin sorulan hâli (klasörde altındaki notlar).
    std::vector<std::vector<ShownNote>> shown;
    for (const TrashTarget& item : items)
        shown.push_back(item.is_folder ? shown_under(listing, item.path.string())
                                       : std::vector{shown_note(listing, item.path)});
    // Klasörde kaybedilecek not yoksa onaysız: tek satırın kuralı.
    if (has_notes_in_folder &&
        !ask(prompts,
             {.kind = ConfirmKind::TrashBatch, .items = items.size(), .notes = note_count(listing, items)}))
        return std::nullopt;
    for (std::vector<ShownNote>& notes : shown)
        apply_written(notes, prompts);
    Step step;
    BatchReport report;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const TrashTarget& item = items[i];
        const std::string path = item.path.string();
        std::optional<core::Error> failure;
        if (item.is_folder) {
            FolderTrash done = trash_shown_folder(store, listing, shown[i], path);
            std::ranges::move(done.notes, std::back_inserter(step.notes));
            std::ranges::move(done.folders, std::back_inserter(step.folders));
            failure = std::move(done.failure);
        } else if (auto trashed = trash_shown(store, shown[i].front())) {
            step.notes.push_back(std::move(*trashed));
        } else {
            failure = trashed.error();
        }
        if (failure)
            report.fail(path, *failure);
        else
            ++report.done;
    }
    report.wrote = !step.notes.empty() || !step.folders.empty();
    if (report.wrote)
        push(std::move(step));
    return report;
}

std::optional<Undone> TrashOps::undo(core::NoteStore& store) {
    if (steps_.empty())
        return std::nullopt;
    const Step& last = steps_.back();
    if (!last.folder.empty() || !last.folders.empty() || last.notes.size() > 1)
        return undo_batch(store);
    Undone result{.single = true};
    const core::TrashItem entry = last.notes.back();
    auto restored = restore_entry(store, entry);
    if (!restored) {
        result.report.fail(entry.path.string(), restored.error());
        if (restored.error().code == core::Error::Code::NotFound)
            steps_.pop_back();  // geri gelemez: eski adımları kilitlemesin
        return result;          // başka hata: adım kalır, kilit kalkınca yeniden denenir
    }
    steps_.pop_back();
    ++result.report.done;
    result.first = *restored;
    return result;
}

// Adımın tamamı: önce klasörler (boş alt klasörler de), sonra notlar. Biri düşerse kalanlar sürer; kalan hata
// yığında kalır, çöpte artık olmayan not adımı kilitlemesin diye düşer.
Undone TrashOps::undo_batch(core::NoteStore& store) {
    const Step step = std::move(steps_.back());
    steps_.pop_back();
    Undone result{.folder = step.folder};
    Step retry{.folders = step.folders, .folder = step.folder};
    for (const std::string& path : step.folders)
        if (auto made = store.create_folder(path); !made)
            result.report.fail(path, made.error());
    for (const core::TrashItem& entry : step.notes | std::views::reverse) {
        auto restored = restore_entry(store, entry);
        if (!restored) {
            result.report.fail(entry.path.string(), restored.error());
            if (restored.error().code != core::Error::Code::NotFound)
                retry.notes.push_back(entry);
            continue;
        }
        ++result.report.done;
        if (*restored != entry.path)
            result.report.renamed(entry.path.string(), restored->string());
        if (!result.first)
            result.first = *restored;
    }
    if (!retry.notes.empty())
        steps_.push_back(std::move(retry));
    return result;
}

std::expected<core::NotePath, core::Error> TrashOps::restore_entry(core::NoteStore& store,
                                                                   const core::TrashItem& entry) {
    const auto items = store.list_trash();
    if (!items)
        return std::unexpected(items.error());
    const auto same = std::ranges::find_if(
        *items, [&](const core::TrashItem& item) { return item.path == entry.path && item.id == entry.id; });
    if (same == items->end())
        return std::unexpected(
            core::Error{core::Error::Code::NotFound, "error.not_found", {{"path", entry.path.string()}}});
    return store.restore(entry.path);
}

namespace {

// Kullanıcının gördüğü notları (sorudan önce alınan hâlleriyle) kalıcı siler, sonra boşalan klasörü kaldırır.
std::expected<void, core::Error> remove_shown_folder(core::NoteStore& store, std::span<const ShownNote> notes,
                                                     const std::string& folder) {
    std::optional<core::Error> changed;  // onaydan sonra değişen ilk not: klasörün kalma nedeni
    for (const ShownNote& note : notes) {
        auto removed = remove_shown(store, note);
        // Zaten yok: silinmek istenen hâl. Onaydan sonra değişen not kalır (klasör de); başka hata klasörü
        // olduğu gibi bırakır.
        if (!removed && removed.error().code == core::Error::Code::ModifiedExternally) {
            changed = changed ? changed : removed.error();
            continue;
        }
        if (!removed && removed.error().code != core::Error::Code::NotFound)
            return removed;
    }
    auto emptied = store.remove_folder(folder, false);
    if (!emptied && emptied.error().code == core::Error::Code::NotEmpty && changed)
        return std::unexpected(std::move(*changed));  // kalan not gösterilmeyen değil, değişen nottur
    if (!emptied && emptied.error().code == core::Error::Code::NotEmpty) {
        core::Error unseen = emptied.error();
        unseen.message_key = "error.folder_unseen_notes";
        return std::unexpected(std::move(unseen));
    }
    return emptied;
}

}  // namespace

std::expected<core::NotePath, core::Error> TrashOps::restore(core::NoteStore& store,
                                                             const core::NotePath& item) {
    auto restored = store.restore(item);
    if (restored)
        forget(item);
    return restored;
}

BatchReport TrashOps::restore_items(core::NoteStore& store, std::span<const core::NotePath> items) {
    BatchReport report;
    for (const core::NotePath& item : items) {
        const auto restored = store.restore(item);
        if (!restored) {
            report.fail(item.string(), restored.error());
            continue;
        }
        ++report.done;
        if (*restored != item)
            report.renamed(item.string(), restored->string());
        forget(item);
    }
    return report;
}

std::expected<bool, core::Error> TrashOps::remove(core::NoteStore& store, const Listing& listing,
                                                  const core::NotePath& path, const Prompts& prompts) {
    const ShownNote shown = shown_note(listing, path);  // onaydan önce: sorulan hâl
    if (!ask(prompts, {.kind = ConfirmKind::Delete, .path = path.string()}))
        return false;
    if (auto removed = remove_shown(store, shown); !removed)
        return std::unexpected(removed.error());
    return true;
}

std::expected<bool, core::Error> TrashOps::purge(core::NoteStore& store, const core::NotePath& item,
                                                 const Prompts& prompts) {
    if (!ask(prompts, {.kind = ConfirmKind::Purge, .path = item.string()}))
        return false;
    if (auto purged = store.purge(item); !purged)
        return std::unexpected(purged.error());
    forget(item);
    return true;
}

std::expected<bool, core::Error> TrashOps::remove_folder(core::NoteStore& store, const Listing& listing,
                                                         std::string_view folder, const Prompts& prompts) {
    const core::FolderNode* node = listing.tree().find(folder);
    const Confirm request{.kind = ConfirmKind::DeleteFolder,
                          .path = std::string(folder),
                          .notes = node ? node->note_count : 0,
                          .folders = node ? node->folder_count : 0};
    const std::vector<ShownNote> shown = shown_under(listing, folder);  // onaydan önce: sayılan notlar
    if (!ask(prompts, request))
        return false;
    if (auto removed = remove_shown_folder(store, shown, std::string(folder)); !removed)
        return std::unexpected(removed.error());
    return true;
}

std::optional<BatchReport> TrashOps::delete_items(core::NoteStore& store, const Listing& listing,
                                                  std::span<const TrashTarget> items, bool from_trash,
                                                  const Prompts& prompts) {
    // Onaydan önce: her öğenin sorulan hâli (klasörde altındaki notlar), sonra soru.
    std::vector<std::vector<ShownNote>> shown;
    for (const TrashTarget& item : items)
        shown.push_back(item.is_folder ? shown_under(listing, item.path.string())
                                       : std::vector{shown_note(listing, item.path)});
    if (!ask(prompts,
             {.kind = ConfirmKind::DeleteBatch, .items = items.size(), .notes = note_count(listing, items)}))
        return std::nullopt;
    BatchReport report;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const TrashTarget& item = items[i];
        const auto removed = [&] {
            if (item.is_folder)
                return remove_shown_folder(store, shown[i], item.path.string());
            return from_trash ? store.purge(item.path) : remove_shown(store, shown[i].front());
        }();
        if (!removed) {
            report.fail(item.path.string(), removed.error());
            continue;
        }
        ++report.done;
        if (from_trash)
            forget(item.path);
    }
    return report;
}

std::expected<bool, core::Error> TrashOps::empty_trash(core::NoteStore& store, const Prompts& prompts) {
    const auto items = store.list_trash();
    if (!ask(prompts, {.kind = ConfirmKind::EmptyTrash, .items = items ? items->size() : 0}))
        return false;
    if (auto emptied = store.empty_trash(); !emptied)
        return std::unexpected(emptied.error());
    steps_.clear();
    return true;
}

}  // namespace noteapp::app
