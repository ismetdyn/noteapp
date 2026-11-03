#include "core/file_stat.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <system_error>
#include <tuple>

namespace noteapp::core {

namespace fs = std::filesystem;
using graph::NodeId;
using graph::PropertyHead;

Error io_error(std::string_view path, std::string_view detail) {
    return Error{Error::Code::Io, "error.io", {{"path", std::string(path)}, {"detail", std::string(detail)}}};
}

Error not_found(std::string_view path) {
    return Error{Error::Code::NotFound, "error.not_found", {{"path", std::string(path)}}};
}

Error invalid_path(std::string_view reason, std::string_view path) {
    return Error{Error::Code::InvalidPath,
                 "error.invalid_path",
                 {{"reason", std::string(reason)}, {"path", std::string(path)}}};
}

Error already_exists(std::string_view path) {
    return Error{Error::Code::AlreadyExists, "error.already_exists", {{"path", std::string(path)}}};
}

Error from_graph(const graph::Error& error, std::string_view path) {
    switch (error.code) {
    case graph::Error::Code::TooNew:
        return Error{Error::Code::Io, "error.graph_too_new", {{"path", std::string(path)}}};
    case graph::Error::Code::Corrupt:
        return Error{Error::Code::Io, "error.graph_corrupt", {{"path", std::string(path)}}};
    default:
        break;
    }
    std::string detail = error.message_key;
    for (const auto& [key, value] : error.args)
        detail.append(" ").append(key).append("=").append(value);
    return io_error(path, detail);
}

std::string path_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string id_hex(const NodeId& id) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    for (const std::byte value : id.bytes) {
        text += kDigits[std::to_integer<unsigned>(value) >> 4U];
        text += kDigits[std::to_integer<unsigned>(value) & 0xFU];
    }
    return text;
}

bool folder_path_within_limits(std::string_view folder) {
    return folder.size() <= kMaxFolderPathBytes &&
           static_cast<std::size_t>(std::ranges::count(folder, '/')) < kMaxFolderDepth;
}

NoteParts split_note_path(std::string_view path) {
    NoteParts parts;
    std::string_view file = path;
    if (const auto slash = path.rfind('/'); slash != std::string_view::npos) {
        parts.folder = std::string(path.substr(0, slash));
        file = path.substr(slash + 1);
    }
    if (const auto dot = file.rfind('.'); dot != std::string_view::npos && dot != 0) {
        parts.name = std::string(file.substr(0, dot));
        parts.type = std::string(file.substr(dot + 1));
    } else {
        parts.name = std::string(file);
    }
    return parts;
}

// "a.tar.gz", 2 -> "a.tar-2.gz": create()'in ad ekiyle aynı biçim.
std::expected<NotePath, Error> numbered_path(const NotePath& path, int number) {
    const NoteParts parts = split_note_path(path.string());
    std::string text = parts.folder.empty() ? std::string() : parts.folder + "/";
    text += parts.name + "-" + std::to_string(number);
    if (!parts.type.empty())
        text += "." + parts.type;
    return NotePath::parse(text);
}

namespace {

fs::file_time_type from_ms(std::int64_t unix_ms) {
    const std::chrono::sys_time<std::chrono::milliseconds> sys{std::chrono::milliseconds(unix_ms)};
    return std::chrono::time_point_cast<fs::file_time_type::duration>(std::chrono::file_clock::from_sys(sys));
}

// Okuma kuralı SPEC-graph ile aynı: en büyük (hlc, cihaz) head'i.
const PropertyHead* best_head(std::span<const PropertyHead> heads, std::string_view key) {
    const PropertyHead* best = nullptr;
    for (const PropertyHead& head : heads) {
        if (head.key != key)
            continue;
        if (best == nullptr || std::tie(head.hlc, head.device) > std::tie(best->hlc, best->device))
            best = &head;
    }
    return best;
}

// Uzun ad (200 baytı aşan: Türkçe 255 bayta kadar) büyük değer olarak durur, toplu okuma yüklemez.
graph::Result<std::string> text_of(graph::ReadTxn& txn, const NodeId& id, const PropertyHead* head) {
    if (head == nullptr)
        return std::string();
    if (head->large) {
        auto value = txn.property(id, head->key);
        if (!value)
            return std::unexpected(value.error());
        const std::string* text = std::get_if<std::string>(&*value);
        return text == nullptr ? std::string() : *text;
    }
    const std::string* text = std::get_if<std::string>(&head->value);
    return text == nullptr ? std::string() : *text;
}

std::optional<std::int64_t> time_of(const PropertyHead* head) {
    const graph::Timestamp* time = head == nullptr ? nullptr : std::get_if<graph::Timestamp>(&head->value);
    return time == nullptr ? std::nullopt : std::optional(time->unix_ms);
}

std::string file_name(const std::string& name, const std::string& type) {
    return type.empty() ? name : name + "." + type;
}

// Kenarı en büyük kimlikli olan seçilir: taşıma kenarı UUIDv7'dir, en son taşıma kazanır. Her cihaz aynı
// kümeden aynı sonucu üretir; eşzamanlı iki taşıma iki kenar bırakır, yazma kuralı bunu bir sonraki taşımada
// temizler.
graph::Result<std::optional<NodeId>> chosen_edge_target(graph::ReadTxn& txn, const NodeId& id,
                                                        std::string_view edge_type) {
    auto cursor = txn.neighbors(id, graph::Direction::Out, edge_type);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::optional<graph::EdgeId> best_edge;
    std::optional<NodeId> best_node;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return best_node;
        if (!best_edge || (*next)->edge > *best_edge) {
            best_edge = (*next)->edge;
            best_node = (*next)->node;
        }
    }
}

graph::Result<std::vector<NodeId>> nodes_labeled(graph::ReadTxn& txn, std::string_view label) {
    auto cursor = txn.nodes_with_label(label);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::vector<NodeId> ids;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return ids;
        ids.push_back(**next);
    }
}

struct FolderNode {
    std::string name;
    std::optional<NodeId> parent;
    std::optional<std::string> path;  // adı ya da atası geçersizse, çok derinse boş
    std::size_t depth = 0;
    bool resolved = false;
};

// Aynı ad en çok bu kadar kopya alır: 100.000 aynı adlı not her görüntüde milyarlarca denemeye dönmesin.
constexpr int kMaxCollisionCopies = 1000;

using FolderMap = std::map<NodeId, FolderNode>;

// CHILD_OF döngüsü (eşzamanlı iki taşıma): döngüdeki en küçük kimlikli klasörün üst kenarı yok sayılır. Çözüm
// kümenin saf fonksiyonudur, gezinti sırasına bağlı değildir.
void break_cycles(FolderMap& folders) {
    std::set<NodeId> done;
    for (const auto& [start, unused] : folders) {
        std::vector<NodeId> trail;
        std::set<NodeId> on_trail;
        std::optional<NodeId> current = start;
        while (current && !done.contains(*current) && !on_trail.contains(*current)) {
            trail.push_back(*current);
            on_trail.insert(*current);
            const auto found = folders.find(*current);
            current = found == folders.end() ? std::nullopt : found->second.parent;
        }
        if (current && on_trail.contains(*current)) {
            const auto cycle_start = std::ranges::find(trail, *current);
            const NodeId cut = *std::min_element(cycle_start, trail.end());
            folders[cut].parent.reset();
        }
        done.insert(trail.begin(), trail.end());
    }
}

// Tek klasörün yolu; üstü çözülmüş olmalı. Ad uzaktan gelebilir: NotePath kapısından geçmeyen yol hiçbir yere
// ulaşmaz (SPEC-graph "Güvenilmez girdi").
void resolve_folder(FolderMap& folders, FolderNode& folder) {
    folder.resolved = true;
    const FolderNode* parent = nullptr;
    if (folder.parent)
        if (const auto found = folders.find(*folder.parent); found != folders.end())
            parent = &found->second;
    if (parent != nullptr && !parent->path)
        return;
    folder.depth = parent == nullptr ? 1 : parent->depth + 1;
    if (folder.name.empty() || folder.name.find('/') != std::string::npos || !NotePath::parse(folder.name) ||
        folder.depth > kMaxFolderDepth)
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- üst doluysa path'i yukarıda denetlendi
    const std::size_t parent_bytes = parent == nullptr ? 0 : parent->path->size() + 1;
    if (parent_bytes + folder.name.size() > kMaxFolderPathBytes)
        return;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- üst doluysa path'i yukarıda denetlendi
    folder.path = parent == nullptr ? folder.name : *parent->path + "/" + folder.name;
}

// Özyinelemesiz: 200.000 halkalı zincir yığını taşırır, üstelik günlükten geri gelip her açılışta çökerdi.
// Döngüler break_cycles'ta kesildi, her yürüyüş köke ya da çözülmüş üste varır.
void resolve_folders(FolderMap& folders) {
    std::vector<FolderNode*> trail;
    for (auto& [id, start] : folders) {
        for (FolderNode* current = &start; current != nullptr && !current->resolved;) {
            trail.push_back(current);
            const auto parent = current->parent ? folders.find(*current->parent) : folders.end();
            current = parent == folders.end() ? nullptr : &parent->second;
        }
        for (; !trail.empty(); trail.pop_back())
            resolve_folder(folders, *trail.back());
    }
}

const std::optional<std::string>& folder_path(const FolderMap& folders, const NodeId& id) {
    return folders.at(id).path;
}

graph::Result<FolderMap> load_folders(graph::ReadTxn& txn) {
    auto ids = nodes_labeled(txn, kFolderLabel);
    if (!ids)
        return std::unexpected(ids.error());
    FolderMap folders;
    for (const NodeId& id : *ids) {
        auto name = txn.property(id, kName);
        if (!name)
            return std::unexpected(name.error());
        auto parent = chosen_edge_target(txn, id, kChildOf);
        if (!parent)
            return std::unexpected(parent.error());
        const std::string* text = std::get_if<std::string>(&*name);
        folders.emplace(id,
                        FolderNode{text == nullptr ? std::string() : *text, *parent, std::nullopt, false});
    }
    break_cycles(folders);
    resolve_folders(folders);
    return folders;
}

// Eşzamanlı oluşturma aynı yolu iki kez verir: önce atılan (çöp) ya da eski not yolu alır, eşitlikte küçük
// kimlik; diğerleri deterministik -2, -3... Sıra yerel saate değil yalnız saklanan değerlere bağlı.
void assign_unique_paths(std::vector<NoteRow>& rows, std::unordered_map<std::string, std::size_t>& index,
                         const std::map<std::string, std::vector<NodeId>>* folders,
                         std::vector<UnreachableNote>& lost) {
    std::ranges::sort(rows, [](const NoteRow& a, const NoteRow& b) {
        const std::string left = a.info.path.string();
        const std::string right = b.info.path.string();
        const auto left_at = a.info.created_at.value_or(fs::file_time_type{});
        const auto right_at = b.info.created_at.value_or(fs::file_time_type{});
        return std::tie(left, a.trashed_at, left_at, a.id) < std::tie(right, b.trashed_at, right_at, b.id);
    });
    const auto taken = [&](const std::string& path) {
        return index.contains(path) || (folders != nullptr && folders->contains(path));
    };
    std::vector<std::size_t> losers;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string path = rows[i].info.path.string();
        if (taken(path))
            losers.push_back(i);
        else
            index.emplace(path, i);
    }
    // Ad başına sayaç sürer: her kaybeden 2'den başlasaydı aynı adlı n not O(n²) deneme ederdi.
    std::unordered_map<std::string, int> next_number;
    std::vector<bool> dropped(rows.size(), false);
    for (const std::size_t i : losers) {
        const std::string base = rows[i].info.path.string();
        int& number = next_number.try_emplace(base, 2).first->second;
        bool placed = false;
        for (; number <= kMaxCollisionCopies + 1 && !placed; ++number) {
            auto candidate = numbered_path(rows[i].info.path, number);
            if (!candidate)
                break;  // ad sınırı
            if (!taken(candidate->string())) {
                index.emplace(candidate->string(), i);
                rows[i].info.path = std::move(*candidate);
                placed = true;
            }
        }
        if (!placed) {
            lost.push_back({id_hex(rows[i].id), base, "too_many_collisions"});
            dropped[i] = true;
        }
    }
    if (std::ranges::find(dropped, true) == dropped.end())
        return;
    std::vector<NoteRow> kept;
    kept.reserve(rows.size());
    index.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (dropped[i])
            continue;
        index.emplace(rows[i].info.path.string(), kept.size());
        kept.push_back(std::move(rows[i]));
    }
    rows = std::move(kept);
}

// Etkili çöp: silme gövdenin her head'ini görmüş olmalı. Görmediği düzenleme eşzamanlıdır ve düzenleme
// kazanır (SPEC-graph "Silme ve düzenleme çakışması"): saf okuma kuralı, her cihaz aynı sonuca varır, yazma
// gerekmez.
graph::Result<bool> effectively_trashed(graph::ReadTxn& txn, const NodeId& id,
                                        std::span<const PropertyHead> heads, const PropertyHead* deleted) {
    if (deleted == nullptr || !std::holds_alternative<graph::Timestamp>(deleted->value))
        return false;
    bool all_seen = true;
    for (const PropertyHead& head : heads) {
        if (head.key != kBody)
            continue;
        auto seen = txn.saw(*deleted, head);
        if (!seen)
            return std::unexpected(seen.error());
        all_seen = all_seen && *seen;
    }
    if (all_seen)
        return true;
    // Görülmeyen head silmenin gördüğüyle aynı baytlarsa düzenleme değildir (iki cihaz aynı dosyayı geçirdi).
    auto versions = txn.versions(id, kBody);
    if (!versions)
        return std::unexpected(versions.error());
    std::vector<const graph::Value*> seen_values;
    std::vector<const graph::Value*> unseen_values;
    for (const graph::Version& version : *versions) {
        auto seen = txn.saw(*deleted, version);
        if (!seen)
            return std::unexpected(seen.error());
        (*seen ? seen_values : unseen_values).push_back(&version.value);
    }
    return std::ranges::all_of(unseen_values, [&](const graph::Value* unseen) {
        return std::ranges::any_of(seen_values, [&](const graph::Value* known) { return *known == *unseen; });
    });
}

struct LoadedNote {
    NoteRow row;
    bool trashed = false;
};

// Ad + tip + klasör kenarı. Yol çıkmıyorsa nedeni lost'a girer, boş döner.
graph::Result<std::optional<std::string>> derive_path(graph::ReadTxn& txn, const NodeId& id,
                                                      std::span<const PropertyHead> heads,
                                                      const FolderMap& folders,
                                                      std::vector<UnreachableNote>& lost) {
    const auto name = text_of(txn, id, best_head(heads, kName));
    const auto type = text_of(txn, id, best_head(heads, kType));
    if (!name || !type)
        return std::unexpected(!name ? name.error() : type.error());
    std::string path = file_name(*name, *type);
    // Ad tek bileşendir: "a/b" NotePath'ten geçer ama gezilmemiş bir klasör uydururdu.
    if (name->empty() || path.find('/') != std::string::npos) {
        lost.push_back({id_hex(id), path, "invalid_name"});
        return std::nullopt;
    }
    auto parent = chosen_edge_target(txn, id, kInFolder);
    if (!parent)
        return std::unexpected(parent.error());
    if (*parent && folders.contains(**parent)) {
        const std::optional<std::string>& folder = folder_path(folders, **parent);
        if (!folder) {
            lost.push_back({id_hex(id), path, "invalid_folder"});
            return std::nullopt;
        }
        path = *folder + "/" + path;
    }
    return std::optional(std::move(path));
}

graph::Result<void> fill_info(graph::ReadTxn& txn, const NodeId& id, std::span<const PropertyHead> heads,
                              const std::array<std::byte, 16>& device, NoteInfo& info, bool& conflicted) {
    if (const PropertyHead* body = best_head(heads, kBody); body != nullptr) {
        info.size = body->size;
        info.revision = Revision{body->device, body->seq};
    }
    if (const auto created = time_of(best_head(heads, kCreatedAt)))
        info.created_at = from_ms(*created);
    if (const PropertyHead* origin = best_head(heads, kNoteOrigin); origin != nullptr) {
        auto text = text_of(txn, id, origin);
        if (!text)
            return std::unexpected(text.error());
        info.origin = std::move(*text);
    }
    std::size_t body_heads = 0;
    for (const PropertyHead& head : heads) {
        body_heads += head.key == kBody ? 1 : 0;
        if (head.device == device) {
            info.local_seq = std::max(info.local_seq, head.seq);
            if (head.key == kCreatedAt)
                info.created_local_seq = head.seq;
        }
    }
    conflicted = body_heads > 1;
    return {};
}

graph::Result<std::optional<LoadedNote>> load_note(graph::ReadTxn& txn, const NodeId& id,
                                                   const FolderMap& folders,
                                                   const std::array<std::byte, 16>& device,
                                                   std::vector<UnreachableNote>& lost) {
    auto heads = txn.properties(id);
    if (!heads)
        return std::unexpected(heads.error());
    auto derived = derive_path(txn, id, *heads, folders, lost);
    if (!derived)
        return std::unexpected(derived.error());
    if (!*derived)
        return std::nullopt;
    std::string path = std::move(**derived);

    const PropertyHead* deleted = best_head(*heads, kDeletedAt);
    auto trashed = effectively_trashed(txn, id, *heads, deleted);
    if (!trashed)
        return std::unexpected(trashed.error());
    if (*trashed) {  // atıldığı yol, klasör sonradan silinmiş ya da taşınmış olsa da geçerlidir
        const auto from = text_of(txn, id, best_head(*heads, kTrashedFrom));
        if (!from)
            return std::unexpected(from.error());
        if (NotePath::parse(*from))
            path = *from;
    }
    auto parsed = NotePath::parse(path);
    if (!parsed) {
        lost.push_back({id_hex(id), path, "invalid_name"});
        return std::nullopt;
    }

    const auto updated = time_of(best_head(*heads, kUpdatedAt)).value_or(0);
    LoadedNote loaded{NoteRow{id, NoteInfo{std::move(*parsed), from_ms(updated), 0}, {}, false, {}},
                      *trashed};
    if (auto filled = fill_info(txn, id, *heads, device, loaded.row.info, loaded.row.conflicted); !filled)
        return std::unexpected(filled.error());
    if (*trashed)
        loaded.row.trashed_at = from_ms(std::get<graph::Timestamp>(deleted->value).unix_ms);
    auto targets = text_of(txn, id, best_head(*heads, kLinkTargets));
    if (!targets)
        return std::unexpected(targets.error());
    loaded.row.link_targets = std::move(*targets);
    return std::optional<LoadedNote>(std::move(loaded));
}

}  // namespace

graph::Result<Snapshot> build_snapshot(graph::ReadTxn& txn, const std::array<std::byte, 16>& device) {
    auto folders = load_folders(txn);
    if (!folders)
        return std::unexpected(folders.error());
    Snapshot snapshot;
    for (const auto& [id, node] : *folders)
        if (const std::optional<std::string>& path = folder_path(*folders, id); path)
            snapshot.folders[*path].push_back(id);

    auto notes = nodes_labeled(txn, kNoteLabel);
    if (!notes)
        return std::unexpected(notes.error());
    snapshot.live.reserve(notes->size());
    for (const NodeId& id : *notes) {
        auto loaded = load_note(txn, id, *folders, device, snapshot.unreachable);
        if (!loaded)
            return std::unexpected(loaded.error());
        if (*loaded)
            ((*loaded)->trashed ? snapshot.trash : snapshot.live).push_back(std::move((*loaded)->row));
    }
    assign_unique_paths(snapshot.live, snapshot.live_by_path, &snapshot.folders, snapshot.unreachable);
    assign_unique_paths(snapshot.trash, snapshot.trash_by_path, nullptr, snapshot.unreachable);
    // Görüntü başına tek satır: 100.000 kötü ad logu doldurmasın. Tam liste unreachable()'dadır.
    if (!snapshot.unreachable.empty())
        log_warn("note store: skipped " + std::to_string(snapshot.unreachable.size()) +
                 " unreachable notes, first: '" + snapshot.unreachable.front().name + "' (" +
                 snapshot.unreachable.front().reason + ")");
    for (std::size_t i = 0; i < snapshot.live.size(); ++i)
        snapshot.live_by_id.emplace(snapshot.live[i].id, i);
    if (auto tagged = load_tags(txn, snapshot); !tagged)
        return std::unexpected(tagged.error());
    snapshot.infos.reserve(snapshot.live.size());
    for (const NoteRow& row : snapshot.live)
        snapshot.infos.push_back(row.info);
    std::ranges::sort(snapshot.infos, [](const NoteInfo& a, const NoteInfo& b) {
        if (a.mtime != b.mtime)
            return a.mtime > b.mtime;
        return a.path.string() < b.path.string();
    });
    return snapshot;
}

std::shared_ptr<const Snapshot> NoteStore::Impl::current() {
    last_error.reset();
    if (!graph) {
        std::error_code ec;
        if (!fs::exists(database(), ec))
            return nullptr;
        auto opened = graph::Graph::open(database());
        if (!opened) {
            last_error = from_graph(opened.error(), path_utf8(database()));
            log_error("note store: cannot open graph: " + last_error->message_key);
            return nullptr;
        }
        graph = std::move(*opened);
        index_derived_once();
    }
    auto txn = graph->read();
    auto fresh = txn ? refresh_in(*txn) : std::unexpected(from_graph(txn.error(), path_utf8(database())));
    if (!fresh) {
        last_error = fresh.error();
        log_error("note store: cannot read graph: " + last_error->message_key);
        return nullptr;
    }
    return *fresh;
}

std::expected<std::shared_ptr<const Snapshot>, Error> NoteStore::Impl::refresh_in(graph::ReadTxn& txn) {
    const auto version = opened_graph().data_version();
    if (!version)
        return std::unexpected(from_graph(version.error(), path_utf8(database())));
    if (snapshot && *version == snapshot_version)
        return snapshot;
    auto built = build_snapshot(txn, opened_graph().device());
    if (!built)
        return std::unexpected(from_graph(built.error(), path_utf8(database())));
    snapshot = std::make_shared<const Snapshot>(std::move(*built));
    snapshot_version = *version;
    return snapshot;
}

std::expected<void, Error> NoteStore::Impl::check_gate() {
    if (gate_open || !legacy_notes_dir)
        return {};
    if (legacy_notes_pending(*legacy_notes_dir, directory))
        return std::unexpected(Error{Error::Code::MigrationRequired,
                                     "error.migration_required",
                                     {{"path", path_utf8(*legacy_notes_dir)}}});
    gate_open = true;
    return {};
}

std::expected<void, Error> NoteStore::Impl::open_for_write() {
    if (graph)
        return {};
    if (const std::error_code made = create_private_directories(directory))
        return std::unexpected(io_error(path_utf8(directory), made.message()));
    auto opened = graph::Graph::open(database());
    if (!opened)
        return std::unexpected(from_graph(opened.error(), path_utf8(database())));
    graph = std::move(*opened);
    index_derived_once();
    return {};
}

std::expected<std::pair<std::string, Revision>, Error> NoteStore::Impl::read_body(const NodeId& id,
                                                                                  std::string_view path) {
    auto txn = opened_graph().read();
    if (!txn)
        return std::unexpected(from_graph(txn.error(), path));
    auto heads = txn->properties(id);
    if (!heads)
        return std::unexpected(from_graph(heads.error(), path));
    const PropertyHead* body = best_head(*heads, kBody);
    if (body == nullptr)
        return std::pair<std::string, Revision>{};
    auto value = txn->property(id, kBody);
    if (!value)
        return std::unexpected(from_graph(value.error(), path));
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*value);
    return std::pair{bytes == nullptr ? std::string() : bytes->data, Revision{body->device, body->seq}};
}

}  // namespace noteapp::core
