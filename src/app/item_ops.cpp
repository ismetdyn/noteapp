#include "app/item_ops.hpp"

#include "app/document.hpp"
#include "core/folder_tree.hpp"
#include "core/text.hpp"

#include <chrono>
#include <utility>

namespace noteapp::app {

namespace {

bool has_marked_ancestor(const std::set<std::string>& marks, std::string_view path) {
    for (std::string folder = core::parent_folder(path); !folder.empty();
         folder = core::parent_folder(folder))
        if (marks.contains(folder + "/"))
            return true;
    return false;
}

core::Error into_itself(std::string_view dest) {
    return {core::Error::Code::InvalidPath,
            "error.invalid_path",
            {{"reason", "into_itself"}, {"path", std::string(dest)}}};
}

// Baytlar aynen (create_exact satır sonuna dokunmaz); ad doluysa deponun `-2` araması. Uzantısız not yeni
// oluşturulamaz (depo tip ister): hata öğenin hatasıdır, kaynak yerindedir.
void paste_note(core::NoteStore& store, const core::NotePath& from, std::string_view dest,
                BatchReport& report) {
    const std::string source = from.string();
    auto content = store.read(from);
    if (!content) {
        report.fail(source, content.error());
        return;
    }
    const std::string name = core::last_component(source);  // split_typed_name görünümü buna bakar
    const core::TypedName typed = core::split_typed_name(name);
    const auto made = store.create_exact(core::NewNote{.folder = std::string(dest),
                                                       .name = std::string(typed.stem),
                                                       .type = std::string(typed.extension),
                                                       .content = content->text});
    if (!made) {
        report.fail(source, made.error());
        return;
    }
    ++report.done;
    report.wrote = true;
    if (made->path.string() != core::join_folder(dest, name))
        report.renamed(source, made->path.string());
}

// Klasörde depo numara aramaz (copy_folder dolu hedefte AlreadyExists verir): ek burada, notla aynı biçimde.
// Tavan yok: dolu ad sonludur, en geç onların sayısı + 1'inci denemede boş ad bulunur.
void paste_folder(core::NoteStore& store, const std::string& folder, std::string_view dest,
                  BatchReport& report) {
    if (dest == folder || core::in_folder(dest, folder)) {
        report.fail(folder, into_itself(dest));
        return;
    }
    const std::string name = core::last_component(folder);
    std::string target;
    std::expected<void, core::Error> copied;
    for (std::size_t attempt = 1;; ++attempt) {
        target = core::join_folder(dest, attempt == 1 ? name : name + "-" + std::to_string(attempt));
        if (target == folder)
            continue;  // kendi üst klasörüne: aynı ad kaynağın kendisidir, depo "kendi içine" der
        copied = store.copy_folder(folder, target);
        if (copied || copied.error().code != core::Error::Code::AlreadyExists)
            break;
    }
    if (!copied) {
        if (copied.error().code == core::Error::Code::PartialCopy)
            report.wrote = true;  // yarım kopya yerinde: senkron da görmeli
        report.fail(folder, copied.error());
        return;
    }
    ++report.done;
    report.wrote = true;
    if (target != core::join_folder(dest, name))
        report.renamed(folder, target);
}

// Hedef klasörler oluşmadan denetlenir: reddedilen taşıma ağaçta iz bırakmasın.
std::optional<core::Error> move_collision(const Listing& listing, std::span<const ItemRef> items,
                                          std::string_view dest) {
    for (const ItemRef& item : items) {
        const std::string source = item.path.string();
        const std::string target = core::join_folder(dest, core::last_component(source));
        if (target == source)
            continue;
        if (item.is_folder && (dest == source || core::in_folder(dest, source)))
            return into_itself(dest);
        const auto parsed = core::NotePath::parse(target);
        if (!parsed)
            return parsed.error();
        if (listing.meta_of(*parsed) || listing.tree().find(target))
            return core::Error{core::Error::Code::AlreadyExists, "error.already_exists", {{"path", target}}};
    }
    return std::nullopt;
}

// Klasörü rename_folder üst klasörleri kurmaz: hedef önce oluşur. Not için depo kendisi kurar.
std::optional<core::Error> place_item(core::NoteStore& store, const ItemRef& item, std::string_view dest,
                                      const core::NotePath& target) {
    if (!item.is_folder) {
        const auto moved = store.move(item.path, target);
        return moved ? std::nullopt : std::optional(moved.error());
    }
    if (!dest.empty())
        if (auto made = store.create_folder(dest); !made)
            return made.error();
    const auto moved = store.rename_folder(item.path.string(), target.string());
    return moved ? std::nullopt : std::optional(moved.error());
}

}  // namespace

std::string mark_key(std::string_view path, bool is_folder) {
    return is_folder ? std::string(path) + "/" : std::string(path);
}

std::vector<ItemRef> marked_items(const std::set<std::string>& marks) {
    std::vector<ItemRef> items;
    for (const std::string& key : marks) {
        const bool is_folder = key.ends_with('/');
        const std::string_view path = std::string_view(key).substr(0, key.size() - (is_folder ? 1 : 0));
        if (has_marked_ancestor(marks, path))
            continue;
        if (auto parsed = core::NotePath::parse(path))
            items.push_back({std::move(*parsed), is_folder});
    }
    return items;
}

BatchReport paste_items(core::NoteStore& store, std::span<const ItemRef> items, std::string_view dest) {
    BatchReport report;
    for (const ItemRef& item : items) {
        if (item.is_folder)
            paste_folder(store, item.path.string(), dest, report);
        else
            paste_note(store, item.path, dest, report);
    }
    return report;
}

bool names_folder(std::string_view text) {
    return text.size() > 1 && text.ends_with('/');
}

std::expected<core::CreatedNote, core::Error> create_named_note(core::NoteStore& store, std::string_view name,
                                                                std::string_view default_folder,
                                                                std::string_view default_type,
                                                                std::size_t slug_max_length) {
    const auto now = std::chrono::system_clock::now();
    auto parsed = parse_draft_name(name, default_folder, now, slug_max_length);
    if (!parsed)
        return std::unexpected(parsed.error());
    std::string type = parsed->type.empty() ? std::string(default_type) : std::move(parsed->type);
    return store.create_exact(core::NewNote{.folder = std::move(parsed->folder),
                                            .name = std::move(parsed->stem),
                                            .type = std::move(type),
                                            .content = {},
                                            .exact_name = true},
                              now);
}

MoveResult move_items(core::NoteStore& store, const Listing& listing, std::span<const ItemRef> items,
                      std::string_view dest) {
    MoveResult result;
    if (!dest.empty())
        if (const auto parsed = core::NotePath::parse(dest); !parsed) {
            result.failure = parsed.error();
            return result;
        }
    if ((result.failure = move_collision(listing, items, dest)))
        return result;
    for (const ItemRef& item : items) {
        const std::string source = item.path.string();
        const auto target = core::NotePath::parse(core::join_folder(dest, core::last_component(source)));
        if (!target) {
            result.failure = target.error();
            return result;
        }
        if (target->string() == source)
            continue;
        if ((result.failure = place_item(store, item, dest, *target)))
            return result;
        ++result.moved;
        if (!result.first)
            result.first = ItemRef{*target, item.is_folder};
    }
    return result;
}

}  // namespace noteapp::app
