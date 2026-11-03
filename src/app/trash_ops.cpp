#include "app/trash_ops.hpp"

#include "core/folder_tree.hpp"

#include <algorithm>
#include <ranges>
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

std::size_t note_count(const Listing& listing, std::span<const TrashTarget> items) {
    std::size_t notes = 0;
    for (const TrashTarget& item : items)
        notes += item.is_folder ? note_count(listing, item.path.string()) : 1;
    return notes;
}

}  // namespace

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

// Depo klasörü not başına çöpe atmaz (remove_folder force notları kalıcı siler): notlar tek tek atılır, sonra
// boşalan klasör kaldırılır. Arada gelen yeni not NotEmpty verir, klasör ve o not yerinde kalır. Hiç not
// atılamadıysa klasörler adıma yazılmaz: geri alma senkronun kaldırdığı klasörü geri getirmesin.
std::optional<core::Error> TrashOps::trash_folder_into(core::NoteStore& store, const Listing& listing,
                                                       Step& step, const std::string& folder) {
    std::optional<core::Error> failure;
    std::size_t trashed_count = 0;
    for (const std::size_t note : listing.tree().notes_under(folder)) {
        auto trashed = store.trash(listing.metas()[note].path);
        if (!trashed) {
            failure = failure ? failure : trashed.error();
            continue;  // kalanlar sürer, klasör yerinde kalır
        }
        step.notes.push_back(std::move(*trashed));
        ++trashed_count;
    }
    if (!failure)
        if (auto removed = store.remove_folder(folder, false); !removed)
            failure = removed.error();
    if (!failure || trashed_count > 0)
        std::ranges::copy(listing.tree().folders_under(folder), std::back_inserter(step.folders));
    return failure;
}

std::optional<FolderTrashed> TrashOps::trash_folder(core::NoteStore& store, const Listing& listing,
                                                    std::string_view folder, const Prompts& prompts) {
    if (const core::FolderNode* node = listing.tree().find(folder); node && node->note_count > 0) {
        const Confirm request{.kind = ConfirmKind::TrashFolder,
                              .path = std::string(folder),
                              .notes = node->note_count,
                              .folders = node->folder_count};
        if (!ask(prompts, request))
            return std::nullopt;
    }
    Step step{.folder = std::string(folder)};
    FolderTrashed result;
    result.failure = trash_folder_into(store, listing, step, step.folder);
    result.notes = step.notes.size();
    if (!step.notes.empty() || !result.failure)
        push(std::move(step));
    return result;
}

std::optional<BatchReport> TrashOps::trash_items(core::NoteStore& store, const Listing& listing,
                                                 std::span<const TrashTarget> items, const Prompts& prompts) {
    const bool has_notes_in_folder = std::ranges::any_of(items, [&](const TrashTarget& item) {
        return item.is_folder && note_count(listing, item.path.string()) > 0;
    });
    // Klasörde kaybedilecek not yoksa onaysız: tek satırın kuralı.
    if (has_notes_in_folder &&
        !ask(prompts,
             {.kind = ConfirmKind::TrashBatch, .items = items.size(), .notes = note_count(listing, items)}))
        return std::nullopt;
    Step step;
    BatchReport report;
    for (const TrashTarget& item : items) {
        const std::string path = item.path.string();
        std::optional<core::Error> failure;
        if (item.is_folder) {
            failure = trash_folder_into(store, listing, step, path);
        } else if (auto trashed = store.trash(item.path)) {
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

std::expected<void, core::Error> TrashOps::remove_shown_folder(core::NoteStore& store, const Listing& listing,
                                                               const std::string& folder) {
    for (const std::size_t note : listing.tree().notes_under(folder)) {
        auto removed = store.remove(listing.metas()[note].path);
        // Zaten yok: silinmek istenen hâl; başka hata klasörü olduğu gibi bırakır.
        if (!removed && removed.error().code != core::Error::Code::NotFound)
            return removed;
    }
    auto emptied = store.remove_folder(folder, false);
    if (!emptied && emptied.error().code == core::Error::Code::NotEmpty) {
        core::Error unseen = emptied.error();
        unseen.message_key = "error.folder_unseen_notes";
        return std::unexpected(std::move(unseen));
    }
    return emptied;
}

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

std::expected<bool, core::Error> TrashOps::remove(core::NoteStore& store, const core::NotePath& path,
                                                  const Prompts& prompts) {
    if (!ask(prompts, {.kind = ConfirmKind::Delete, .path = path.string()}))
        return false;
    if (auto removed = store.remove(path); !removed)
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
    if (!ask(prompts, request))
        return false;
    if (auto removed = remove_shown_folder(store, listing, std::string(folder)); !removed)
        return std::unexpected(removed.error());
    return true;
}

std::optional<BatchReport> TrashOps::delete_items(core::NoteStore& store, const Listing& listing,
                                                  std::span<const TrashTarget> items, bool from_trash,
                                                  const Prompts& prompts) {
    if (!ask(prompts,
             {.kind = ConfirmKind::DeleteBatch, .items = items.size(), .notes = note_count(listing, items)}))
        return std::nullopt;
    BatchReport report;
    for (const TrashTarget& item : items) {
        const auto removed = [&] {
            if (item.is_folder)
                return remove_shown_folder(store, listing, item.path.string());
            return from_trash ? store.purge(item.path) : store.remove(item.path);
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
