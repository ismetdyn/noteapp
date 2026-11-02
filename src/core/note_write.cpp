#include "core/note_store_impl.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <map>
#include <set>
#include <tuple>

namespace noteapp::core {

using graph::EdgeId;
using graph::NodeId;

namespace {

constexpr std::size_t kMaxLinksPerNote =
    1024;  // işlem başına op sınırına düşmesin; fazlası bağlantısız kalır
constexpr std::size_t kMaxLinkTargetBytes = 512;
constexpr unsigned kHlcCounterBits = 16;  // hlc = ms << 16 | sayaç (SPEC-graph, tasarım)
constexpr std::size_t kMaxFileNameBytes = 255;
constexpr std::size_t kMaxExtensionBytes = 32;  // bundan uzunu uzantı sayılmaz: damgayı sığdırmak gerekir

std::int64_t to_ms(std::chrono::system_clock::time_point when) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
}

std::span<const std::byte> as_bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string id_text(const NodeId& id) {
    return std::string(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
}

// UTC: kopyanın adı iki cihazda aynı olmalı, yerel saat kimliği ayrıştırırdı.
std::string utc_minute_stamp(std::int64_t unix_ms) {
    const std::time_t seconds = unix_ms / 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}{:02}", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min);
}

std::string cut_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes)
        return text;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)  // 10xxxxxx: devam baytı
        --end;
    return text.substr(0, end);
}

// `<ad> (conflict YYYY-MM-DD HHMM)`; 255 bayta sığacak şekilde.
std::string conflict_name(std::string name, std::string& type, std::int64_t unix_ms) {
    if (type.size() > kMaxExtensionBytes) {  // gerçek uzantı değil; damgaya yer kalsın
        name += "." + type;
        type.clear();
    }
    const std::string tail = std::format(" (conflict {})", utc_minute_stamp(unix_ms));
    const std::size_t extension = type.empty() ? 0 : type.size() + 1;
    return cut_utf8(name, kMaxFileNameBytes - tail.size() - extension) + tail;
}

std::optional<graph::Value> get_value(graph::ReadTxn& txn, const NodeId& id, std::string_view key) {
    auto value = txn.property(id, key);
    return value ? std::optional(std::move(*value)) : std::nullopt;
}

std::string text_value(const std::optional<graph::Value>& value) {
    const std::string* text = value ? std::get_if<std::string>(&*value) : nullptr;
    return text == nullptr ? std::string() : *text;
}

}  // namespace

std::vector<std::string> scan_links(std::string_view body) {
    std::vector<std::string> links;
    std::size_t at = 0;
    while (links.size() < kMaxLinksPerNote) {
        const auto open = body.find("[[", at);
        if (open == std::string_view::npos)
            break;
        const auto close = body.find("]]", open + 2);
        if (close == std::string_view::npos)
            break;
        std::string_view target = body.substr(open + 2, close - open - 2);
        at = close + 2;
        if (target.size() > kMaxLinkTargetBytes || target.find('\n') != std::string_view::npos)
            continue;
        target = target.substr(0, target.find('|'));
        target = target.substr(0, target.find('#'));
        while (target.starts_with(' '))
            target.remove_prefix(1);
        while (target.ends_with(' '))
            target.remove_suffix(1);
        if (!target.empty())
            links.emplace_back(target);
    }
    std::ranges::sort(links);
    const auto duplicates = std::ranges::unique(links);
    links.erase(duplicates.begin(), duplicates.end());
    return links;
}

// Yol, uzantısız yol, dosya adı ve yalın ad: en kesin ilk, her düzeyde yol sırasında ilk gelen kazanır
// (deterministik).
const std::unordered_map<std::string, NodeId>& WriteContext::link_index() {
    if (link_index_)
        return *link_index_;
    link_index_.emplace();
    for (const NoteRow& row : snapshot.live)
        link_index_->emplace(row.info.path.string(), row.id);
    for (const NoteRow& row : snapshot.live) {
        const std::string path = row.info.path.string();
        const NoteParts parts = split_note_path(path);
        if (!parts.type.empty())
            link_index_->emplace(path.substr(0, path.size() - parts.type.size() - 1), row.id);
    }
    for (const NoteRow& row : snapshot.live) {
        const NoteParts parts = split_note_path(row.info.path.string());
        link_index_->emplace(parts.type.empty() ? parts.name : parts.name + "." + parts.type, row.id);
    }
    for (const NoteRow& row : snapshot.live)
        link_index_->emplace(split_note_path(row.info.path.string()).name, row.id);
    return *link_index_;
}

// Hedefler yazma anında çözülür; sonradan oluşan hedef, kaynak bir daha kaydedilene dek bağlanmaz.
// ponytail: tam yeniden tarama yok, geçiş sonunda ve kaynak kaydında bağlanır.
std::expected<void, Error> WriteContext::update_links(const NodeId& id, std::string_view path,
                                                      std::string_view body) {
    std::set<NodeId> wanted;
    if (!looks_binary(body) && body.find("[[") != std::string_view::npos) {
        const auto& index = link_index();
        for (const std::string& target : scan_links(body))
            if (const auto found = index.find(target); found != index.end() && found->second != id)
                wanted.insert(found->second);
    }
    auto cursor = txn.neighbors(id, graph::Direction::Out, kLinksTo);
    if (!cursor)
        return std::unexpected(from_graph(cursor.error(), path));
    std::vector<std::pair<EdgeId, NodeId>> existing;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(from_graph(next.error(), path));
        if (!*next)
            break;
        existing.emplace_back((*next)->edge, (*next)->node);
    }
    for (const auto& [edge, node] : existing) {
        if (wanted.erase(node) > 0)
            continue;
        if (auto gone = txn.delete_edge(edge); !gone)
            return std::unexpected(from_graph(gone.error(), path));
    }
    for (const NodeId& target : wanted) {
        const std::string canonical = id_text(id) + id_text(target);
        const EdgeId edge = graph::derive_edge_id(kLinksTo, as_bytes(canonical));
        if (auto put = txn.put_edge(edge, id, kLinksTo, target); !put)
            return std::unexpected(from_graph(put.error(), path));
    }
    return {};
}

std::expected<std::optional<NodeId>, Error> WriteContext::ensure_folder(std::string_view path) {
    std::optional<NodeId> parent;
    if (path.empty())
        return parent;
    if (!folder_path_within_limits(path))
        return std::unexpected(invalid_path("invalid_folder", path));
    std::size_t at = 0;
    while (at <= path.size()) {
        const auto slash = path.find('/', at);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string current(path.substr(0, end));
        const std::string name(path.substr(at, end - at));
        at = end + 1;
        if (snapshot.live_by_path.contains(current))
            return std::unexpected(io_error(path, "not a directory"));
        if (const auto known = snapshot.folders.find(current); known != snapshot.folders.end()) {
            parent = known->second.front();
            continue;
        }
        if (const auto added = added_folders_.find(current); added != added_folders_.end()) {
            parent = added->second;
            continue;
        }
        auto id = add_folder_node(parent, name, path);
        if (!id)
            return std::unexpected(id.error());
        added_folders_.emplace(current, *id);
        parent = *id;
    }
    return parent;
}

std::expected<NodeId, Error> WriteContext::add_folder_node(const std::optional<NodeId>& parent,
                                                           const std::string& name, std::string_view path) {
    // Kimlik (ata, ad)'dan türer: iki cihazın aynı klasörü aynı düğümdür. Yeniden adlandırılmış düğüm bu
    // kimliği tutuyor olabilir: o zaman sıradaki tuz denenir, aynı düğüm iki yola bağlanmaz.
    std::optional<NodeId> id;
    for (int salt = 0; !id; ++salt) {
        const std::string canonical =
            (parent ? id_text(*parent) : std::string()) + '\0' + name + '\0' + std::to_string(salt);
        const NodeId candidate = graph::derive_node_id(kFolderLabel, as_bytes(canonical));
        auto exists = txn.has_node(candidate);
        if (!exists)
            return std::unexpected(from_graph(exists.error(), path));
        if (!*exists)
            id = candidate;
    }
    if (auto made = txn.put_node(*id); !made)
        return std::unexpected(from_graph(made.error(), path));
    if (auto label = txn.add_label(*id, kFolderLabel); !label)
        return std::unexpected(from_graph(label.error(), path));
    if (auto named = txn.set_property(*id, kName, graph::Value{name}); !named)
        return std::unexpected(from_graph(named.error(), path));
    if (auto linked = set_folder(*id, parent, kChildOf, path); !linked)
        return std::unexpected(linked.error());
    return *id;
}

std::expected<void, Error> WriteContext::set_folder(const NodeId& id, const std::optional<NodeId>& folder,
                                                    std::string_view edge_type, std::string_view path) {
    auto cursor = txn.neighbors(id, graph::Direction::Out, edge_type);
    if (!cursor)
        return std::unexpected(from_graph(cursor.error(), path));
    std::vector<EdgeId> edges;
    bool already_there = false;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(from_graph(next.error(), path));
        if (!*next)
            break;
        edges.push_back((*next)->edge);
        already_there = already_there || (folder && (*next)->node == *folder);
    }
    if (already_there && edges.size() == 1)
        return {};
    for (const EdgeId& edge : edges)
        if (auto gone = txn.delete_edge(edge); !gone)
            return std::unexpected(from_graph(gone.error(), path));
    if (folder)
        if (auto put = txn.put_edge(graph::new_edge_id(), id, edge_type, *folder); !put)
            return std::unexpected(from_graph(put.error(), path));
    return {};
}

std::expected<void, Error> WriteContext::set_text_if_changed(const NodeId& id, std::string_view key,
                                                             const std::string& value,
                                                             std::string_view path) {
    auto current = txn.property(id, key);
    if (!current)
        return std::unexpected(from_graph(current.error(), path));
    if (const std::string* text = std::get_if<std::string>(&*current); text != nullptr && *text == value)
        return {};
    if (auto set = txn.set_property(id, key, graph::Value{value}); !set)
        return std::unexpected(from_graph(set.error(), path));
    return {};
}

std::expected<void, Error> WriteContext::place_note(const NodeId& id, const NotePath& target) {
    const std::string path = target.string();
    const NoteParts parts = split_note_path(path);
    if (auto named = set_text_if_changed(id, kName, parts.name, path); !named)
        return named;
    if (auto typed = set_text_if_changed(id, kType, parts.type, path); !typed)
        return typed;
    auto folder = ensure_folder(parts.folder);
    if (!folder)
        return std::unexpected(folder.error());
    return set_folder(id, *folder, kInFolder, path);
}

std::expected<void, Error> WriteContext::init_note(const NodeId& id, const NotePath& path,
                                                   std::string_view bytes, std::int64_t created_ms,
                                                   std::int64_t updated_ms, std::string_view origin) {
    const std::string text = path.string();
    const auto ok = [&](const graph::Result<void>& step) { return step.has_value(); };
    graph::Result<void> step = txn.put_node(id);
    if (ok(step))
        step = txn.add_label(id, kNoteLabel);
    if (ok(step))
        step = txn.set_property(id, kBody, graph::Value{graph::Bytes{std::string(bytes)}});
    if (ok(step))
        step = txn.set_property(id, kCreatedAt, graph::Value{graph::Timestamp{created_ms}});
    if (ok(step))
        step = txn.set_property(id, kUpdatedAt, graph::Value{graph::Timestamp{updated_ms}});
    if (ok(step))
        step = txn.set_property(id, kNoteOrigin, graph::Value{std::string(origin)});
    if (!ok(step))
        return std::unexpected(from_graph(step.error(), text));
    return place_note(id, path);
}

std::expected<NodeId, Error> WriteContext::create_note(const NotePath& path, std::string_view bytes,
                                                       std::chrono::system_clock::time_point now,
                                                       std::string_view origin) {
    const NodeId id = graph::new_node_id();
    const std::int64_t at = to_ms(now);
    if (auto made = init_note(id, path, bytes, at, at, origin); !made)
        return std::unexpected(made.error());
    if (auto linked = update_links(id, path.string(), bytes); !linked)
        return std::unexpected(linked.error());
    return id;
}

std::expected<Revision, Error> WriteContext::body_revision(const NodeId& id, std::string_view path) {
    auto versions = txn.properties(id);
    if (!versions)
        return std::unexpected(from_graph(versions.error(), path));
    Revision best;
    std::uint64_t best_hlc = 0;
    for (const graph::PropertyHead& head : *versions) {
        if (head.key != kBody)
            continue;
        if (std::tie(head.hlc, head.device) >= std::tie(best_hlc, best.device)) {
            best = Revision{head.device, head.seq};
            best_hlc = head.hlc;
        }
    }
    return best;
}

std::expected<Revision, Error> WriteContext::write_body(const NoteRow& note, std::string_view content,
                                                        std::chrono::system_clock::time_point now) {
    const std::string path = note.info.path.string();
    if (note.conflicted)  // görülen head'leri ezmeden önce: yoksa kaybeden sürüm sessizce giderdi
        if (auto resolved = resolve_body_conflict(note.id); !resolved)
            return std::unexpected(resolved.error());
    auto current = txn.property(note.id, kBody);
    if (!current)
        return std::unexpected(from_graph(current.error(), path));
    if (const graph::Bytes* bytes = std::get_if<graph::Bytes>(&*current);
        bytes != nullptr && bytes->data == content)
        return body_revision(note.id, path);  // aynı baytlar: günlük büyümesin (otomatik kayıt böyle yazmaz)
    NOTEAPP_TRY(set_body_parents(note.id, path));
    if (auto set = txn.set_property(note.id, kBody, graph::Value{graph::Bytes{std::string(content)}}); !set)
        return std::unexpected(from_graph(set.error(), path));
    if (auto set = txn.set_property(note.id, kUpdatedAt, graph::Value{graph::Timestamp{to_ms(now)}}); !set)
        return std::unexpected(from_graph(set.error(), path));
    if (auto linked = update_links(note.id, path, content); !linked)
        return std::unexpected(linked.error());
    return body_revision(note.id, path);
}

std::expected<void, Error> WriteContext::delete_node(const NodeId& id, std::string_view path) {
    if (auto gone = txn.delete_node(id, true); !gone)
        return std::unexpected(from_graph(gone.error(), path));
    return {};
}

std::expected<void, Error> WriteContext::mark_trashed(const NodeId& id, std::string_view path,
                                                      std::chrono::system_clock::time_point now) {
    if (auto set = txn.set_property(id, kDeletedAt, graph::Value{graph::Timestamp{to_ms(now)}}); !set)
        return std::unexpected(from_graph(set.error(), path));
    if (auto set = txn.set_property(id, kTrashedFrom, graph::Value{std::string(path)}); !set)
        return std::unexpected(from_graph(set.error(), path));
    return {};
}

std::expected<void, Error> WriteContext::clear_trashed(const NodeId& id, std::string_view path) {
    if (auto set = txn.set_property(id, kDeletedAt, graph::Value{}); !set)
        return std::unexpected(from_graph(set.error(), path));
    if (auto set = txn.set_property(id, kTrashedFrom, graph::Value{}); !set)
        return std::unexpected(from_graph(set.error(), path));
    return {};
}

std::expected<void, Error> WriteContext::rename_folder(std::span<const NodeId> ids, std::string_view target) {
    const NoteParts parts = [&] {
        NoteParts split;
        const auto slash = target.rfind('/');
        split.folder = slash == std::string_view::npos ? std::string() : std::string(target.substr(0, slash));
        split.name =
            slash == std::string_view::npos ? std::string(target) : std::string(target.substr(slash + 1));
        return split;
    }();
    auto parent = ensure_folder(parts.folder);
    if (!parent)
        return std::unexpected(parent.error());
    for (const NodeId& id : ids) {
        if (auto named = set_text_if_changed(id, kName, parts.name, target); !named)
            return named;
        if (auto moved = set_folder(id, *parent, kChildOf, target); !moved)
            return moved;
    }
    return {};
}

// Kopyanın yolu: asıl notun klasörü, `<ad> (conflict ...).<tip>`. Ad kapıdan geçmiyorsa (hostile ya da
// çöpteki gizli ad) kimlikten türetilmiş güvenli ad: çakışma hiçbir adla ingest'i durduramaz.
std::expected<NotePath, Error> WriteContext::conflict_copy_path(const NodeId& id, const NodeId& copy,
                                                                std::int64_t at) {
    std::string type = text_value(get_value(txn, id, kType));
    const std::string name = text_value(get_value(txn, id, kName));
    std::string file = conflict_name(name, type, at);
    if (!type.empty())
        file += "." + type;
    const auto source = std::ranges::find(snapshot.live, id, &NoteRow::id);
    const std::string folder =
        source != snapshot.live.end() ? split_note_path(source->info.path.string()).folder : std::string();
    if (file.find('/') == std::string::npos) {
        if (auto path = NotePath::parse(folder.empty() ? file : folder + "/" + file))
            return path;
    }
    std::string none;
    std::string fallback = "conflict-";
    for (std::size_t i = 0; i < 8; ++i)
        fallback += std::format("{:02x}", std::to_integer<unsigned>(copy.bytes[i]));
    fallback = conflict_name(fallback, none, at) + ".txt";
    return NotePath::parse(folder.empty() ? fallback : folder + "/" + fallback);
}

// Kaybeden head bir geçiş yazması ve aynı baytları başka cihaz da geçirdiyse o bir düzenleme değil, ortak
// tabandır: başka cihazın gerçek düzenlemesi onu geçmiştir. Kopya üretmek bayat içeriği geri getirirdi.
std::expected<bool, Error> WriteContext::is_stale_import(const NodeId& id, const graph::Version& version) {
    auto digests = txn.versions(id, kMigratedSha);
    if (!digests)
        return std::unexpected(from_graph(digests.error(), "conflict"));
    const auto own = std::ranges::find_if(*digests, [&](const graph::Version& digest) {
        return digest.device == version.device && digest.seq == version.seq;
    });
    if (own == digests->end())
        return false;
    return std::ranges::any_of(*digests, [&](const graph::Version& digest) {
        return digest.value == own->value && (digest.device != version.device || digest.seq != version.seq);
    });
}

// Yazmanın zamanı: aynı çerçevedeki updated_at, yoksa HLC. Geçiş çerçevesinin HLC'si en düşüktür, zaman
// taşımaz.
std::expected<std::int64_t, Error> WriteContext::version_time(const NodeId& id,
                                                              const graph::Version& version) {
    auto times = txn.versions(id, kUpdatedAt);
    if (!times)
        return std::unexpected(from_graph(times.error(), "conflict"));
    for (const graph::Version& time : *times)
        if (time.device == version.device && time.seq == version.seq)
            if (const auto* stamp = std::get_if<graph::Timestamp>(&time.value))
                return stamp->unix_ms;
    return static_cast<std::int64_t>(version.hlc >> kHlcCounterBits);
}

namespace {

// Taban aramasının bütçesi (deterministik: her cihaz aynı yerde durur, aşımda kopya).
constexpr std::size_t kMaxBaseWalkWrites = 4096;
constexpr std::uint64_t kMaxBaseWalkBytes = std::uint64_t{256} * 1024U * 1024U;
constexpr std::size_t kDotBytes = 24;

using Dot = std::tuple<std::array<std::byte, 16>, std::uint64_t>;

std::vector<Dot> decode_parents(const graph::Value& value) {
    std::vector<Dot> parents;
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&value);
    if (bytes == nullptr || bytes->data.size() % kDotBytes != 0)
        return parents;  // uzaktan gelebilir: bozuksa taban yok sayılır
    const auto raw = as_bytes(bytes->data);
    for (std::size_t at = 0; at < raw.size(); at += kDotBytes) {
        Dot dot{};
        std::ranges::copy(raw.subspan(at, 16), std::get<0>(dot).begin());
        for (std::size_t i = 0; i < 8; ++i)
            std::get<1>(dot) |= std::to_integer<std::uint64_t>(raw[at + 16 + i]) << (i * 8U);
        parents.push_back(dot);
    }
    return parents;
}

bool is_mergeable_text(const graph::Value& value) {
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&value);
    return bytes != nullptr && !looks_binary(bytes->data) && is_valid_utf8(bytes->data);
}

}  // namespace

// Bu notun gövdesini yazan her head, kendinden önce gördüğü head'leri kaydeder.
std::expected<void, Error> WriteContext::set_body_parents(const NodeId& id, std::string_view path) {
    auto heads = txn.properties(id);
    if (!heads)
        return std::unexpected(from_graph(heads.error(), path));
    std::string encoded;
    for (const graph::PropertyHead& head : *heads) {
        if (head.key != kBody)
            continue;
        encoded.append(reinterpret_cast<const char*>(head.device.data()), head.device.size());
        for (std::size_t i = 0; i < 8; ++i)
            encoded.push_back(static_cast<char>((head.seq >> (i * 8U)) & 0xFFU));
    }
    NOTEAPP_TRY_GRAPH(txn.set_property(id, kBodyParents, graph::Value{graph::Bytes{std::move(encoded)}}),
                      path);
    return {};
}

// Ortak nedensel ata: her head'in atalarının kesişiminde en yeni yazma (hlc, cihaz, seq). Geçmiş işlem
// günlüğündedir.
namespace {

struct AncestorWalk {
    std::map<Dot, std::size_t> reach;  // kaç head'in atası
    std::map<Dot, std::uint64_t> hlc_of;
    std::uint64_t read_bytes = 0;
    std::size_t read_writes = 0;
};

// Bir head'in gövde ataları. Dönüş: bütçe aşıldıysa false.
graph::Result<bool> walk_ancestors(graph::ReadTxn& txn, const NodeId& id, const graph::Version& head,
                                   AncestorWalk& walk) {
    std::vector<Dot> pending{Dot{head.device, head.seq}};
    std::set<Dot> seen;
    while (!pending.empty()) {
        const Dot dot = pending.back();
        pending.pop_back();
        if (!seen.insert(dot).second)
            continue;
        if (++walk.read_writes > kMaxBaseWalkWrites || walk.read_bytes > kMaxBaseWalkBytes)
            return false;
        graph::Version write;
        write.device = std::get<0>(dot);
        write.seq = std::get<1>(dot);
        auto props = txn.properties_at(write, id);
        if (!props)
            return std::unexpected(props.error());
        for (const graph::PropertyHead& prop : *props) {
            walk.read_bytes += prop.size;
            if (prop.key == kBody) {
                ++walk.reach[dot];
                walk.hlc_of[dot] = prop.hlc;
            } else if (prop.key == kBodyParents) {
                const auto parents = decode_parents(prop.value);
                pending.insert(pending.end(), parents.begin(), parents.end());
            }
        }
    }
    return true;
}

}  // namespace

// Ortak nedensel ata: her head'in atalarının kesişiminde en yeni yazma (hlc, cihaz, seq). Geçmiş işlem
// günlüğündedir.
std::expected<std::optional<std::string>, Error>
WriteContext::common_base(const NodeId& id, std::span<const graph::Version* const> heads) {
    AncestorWalk walk;
    for (const graph::Version* head : heads) {
        auto within = walk_ancestors(txn, id, *head, walk);
        if (!within)
            return std::unexpected(from_graph(within.error(), "conflict"));
        if (!*within)
            return std::optional<std::string>{};
    }
    std::optional<Dot> best;
    for (const auto& [dot, count] : walk.reach) {
        if (count != heads.size())
            continue;
        if (!best || std::tie(walk.hlc_of[dot], dot) > std::tie(walk.hlc_of[*best], *best))
            best = dot;
    }
    if (!best)
        return std::optional<std::string>{};
    graph::Version write;
    write.device = std::get<0>(*best);
    write.seq = std::get<1>(*best);
    auto props = txn.properties_at(write, id);
    if (!props)
        return std::unexpected(from_graph(props.error(), "conflict"));
    for (const graph::PropertyHead& prop : *props)
        if (prop.key == kBody && is_mergeable_text(prop.value))
            return std::optional(std::get<graph::Bytes>(prop.value).data);
    return std::optional<std::string>{};
}

std::expected<std::optional<std::string>, Error>
WriteContext::try_merge(const NodeId& id, std::span<const graph::Version> versions,
                        const TextMerger& merger) {
    // Sıra (cihaz, seq): iki cihaz aynı head'lerden aynı baytı üretir. Aynı baytlı head tek girdidir, bayat
    // geçiş girdi değil.
    std::vector<const graph::Version*> inputs;
    for (const graph::Version& version : versions) {
        if (std::ranges::any_of(inputs,
                                [&](const graph::Version* known) { return known->value == version.value; }))
            continue;
        auto stale = is_stale_import(id, version);
        if (!stale)
            return std::unexpected(stale.error());
        if (*stale)
            continue;
        if (!is_mergeable_text(version.value))
            return std::optional<std::string>{};
        inputs.push_back(&version);
    }
    if (inputs.size() < 2)
        return std::optional<std::string>{};
    auto base = common_base(id, inputs);
    if (!base)
        return std::unexpected(base.error());
    if (!*base)
        return std::optional<std::string>{};
    std::string merged = std::get<graph::Bytes>(inputs.front()->value).data;
    for (std::size_t i = 1; i < inputs.size(); ++i) {
        auto next = merger(**base, merged, std::get<graph::Bytes>(inputs[i]->value).data);
        if (!next)
            return std::optional<std::string>{};
        merged = std::move(*next);
    }
    return std::optional(std::move(merged));
}

// Birleşim temizse gövdeyi yazar; dönüş: yazıldı mı.
std::expected<bool, Error> WriteContext::apply_merge(const NodeId& id,
                                                     std::span<const graph::Version> versions,
                                                     const TextMerger& merger) {
    auto merged = try_merge(id, versions, merger);
    if (!merged)
        return std::unexpected(merged.error());
    if (!*merged)
        return false;
    NOTEAPP_TRY(set_body_parents(id, "body"));
    NOTEAPP_TRY_GRAPH(txn.set_property(id, kBody, graph::Value{graph::Bytes{**merged}}), "body");
    const auto source = std::ranges::find(snapshot.live, id, &NoteRow::id);
    if (source != snapshot.live.end())
        NOTEAPP_TRY(update_links(id, source->info.path.string(), **merged));
    return true;
}

// Kaybeden head'in kopya notu; zaten varsa boş.
std::expected<std::optional<NotePath>, Error> WriteContext::copy_losing_head(const NodeId& id,
                                                                             const graph::Version& version) {
    std::string canonical = id_text(id);
    canonical.append(reinterpret_cast<const char*>(version.device.data()), version.device.size());
    canonical += std::to_string(version.seq);
    const NodeId copy = graph::derive_node_id("conflict", as_bytes(canonical));
    auto exists = txn.has_node(copy);
    if (!exists)
        return std::unexpected(from_graph(exists.error(), "conflict"));
    if (*exists)
        return std::optional<NotePath>{};
    const auto at = version_time(id, version);
    if (!at)
        return std::unexpected(at.error());
    auto path = conflict_copy_path(id, copy, *at);
    if (!path)
        return std::unexpected(path.error());
    const graph::Bytes* bytes = std::get_if<graph::Bytes>(&version.value);
    const std::string origin = text_value(get_value(txn, id, kNoteOrigin));
    NOTEAPP_TRY(init_note(copy, *path, bytes == nullptr ? std::string_view() : std::string_view(bytes->data),
                          *at, *at, origin));
    return std::optional(std::move(*path));
}

std::expected<WriteContext::ConflictStep, Error>
WriteContext::resolve_conflict_step(const NodeId& id, const TextMerger& merger) {
    auto versions = txn.versions(id, kBody);
    if (!versions)
        return std::unexpected(from_graph(versions.error(), "body"));
    if (versions->size() < 2)
        return ConflictStep{};
    const graph::Version& winner =
        *std::ranges::max_element(*versions, [](const graph::Version& a, const graph::Version& b) {
            return std::tie(a.hlc, a.device) < std::tie(b.hlc, b.device);
        });
    if (merger) {
        auto merged = apply_merge(id, *versions, merger);
        if (!merged)
            return std::unexpected(merged.error());
        if (*merged)
            return ConflictStep{};
    }
    bool differ = false;
    for (const graph::Version& version : *versions) {
        if (version.value == winner.value)
            continue;
        differ = true;
        auto stale = is_stale_import(id, version);
        if (!stale)
            return std::unexpected(stale.error());
        if (*stale)
            continue;
        auto copied = copy_losing_head(id, version);
        if (!copied)
            return std::unexpected(copied.error());
        if (*copied)
            return ConflictStep{std::move(*copied), true};
    }
    if (differ) {
        NOTEAPP_TRY(set_body_parents(id, "body"));
        NOTEAPP_TRY_GRAPH(txn.set_property(id, kBody, winner.value), "body");
    }
    return ConflictStep{};
}

std::expected<std::vector<NotePath>, Error> WriteContext::resolve_body_conflict(const NodeId& id) {
    std::vector<NotePath> copies;
    while (true) {
        auto step = resolve_conflict_step(id);
        if (!step)
            return std::unexpected(step.error());
        if (step->copy)
            copies.push_back(std::move(*step->copy));
        if (!step->more)
            return copies;
    }
}

namespace {

std::string import_key(const ImportedNote& note) {
    return (note.deleted_at ? "trash/" : "live/") + note.path.string();
}

graph::Bytes digest_of(std::string_view bytes) {
    const auto digest = graph::sha256(as_bytes(bytes));
    return graph::Bytes{std::string(reinterpret_cast<const char*>(digest.data()), digest.size())};
}

NodeId import_id(const ImportedNote& note) {
    return graph::derive_node_id("migrated-note", as_bytes(import_key(note)));
}

NodeId import_copy_id(const ImportedNote& note, const graph::Bytes& digest) {
    return graph::derive_node_id("migrated-conflict", as_bytes(import_key(note) + digest.data));
}

}  // namespace

std::array<std::string, 2> imported_note_ids(const ImportedNote& note) {
    return {id_hex(import_id(note)), id_hex(import_copy_id(note, digest_of(note.bytes)))};
}

std::expected<bool, Error> verify_import(graph::ReadTxn& txn, const ImportedNote& note) {
    const std::string path = note.path.string();
    const graph::Bytes digest = digest_of(note.bytes);
    for (const NodeId& id : {import_id(note), import_copy_id(note, digest)}) {
        auto body = txn.property(id, kBody);
        if (!body)
            return std::unexpected(from_graph(body.error(), path));
        const graph::Bytes* stored = std::get_if<graph::Bytes>(&*body);
        if (stored != nullptr && stored->data == note.bytes)
            return true;
        auto base = txn.property(id, kMigratedSha);
        if (!base)
            return std::unexpected(from_graph(base.error(), path));
        if (const graph::Bytes* sha = std::get_if<graph::Bytes>(&*base); sha != nullptr && *sha == digest)
            return true;
    }
    return false;
}

std::expected<void, Error> WriteContext::import_existing(const ImportedNote& note, const NodeId& id,
                                                         ImportOutcome& outcome) {
    const std::string path = note.path.string();
    const graph::Bytes digest = digest_of(note.bytes);
    auto base = txn.property(id, kMigratedSha);
    auto body = txn.property(id, kBody);
    if (!base || !body)
        return std::unexpected(from_graph(!base ? base.error() : body.error(), path));
    const graph::Bytes* current = std::get_if<graph::Bytes>(&*body);
    const graph::Bytes* imported_base = std::get_if<graph::Bytes>(&*base);
    const bool same_bytes = current != nullptr && current->data == note.bytes;
    const bool same_base = imported_base != nullptr && *imported_base == digest;
    if (same_bytes || same_base) {
        ++outcome.unchanged;
        return {};
    }
    // Grafta başka içerik var (başka cihaz düzenlemiş): dosyanın sürümü ezmez, yanına kopya olur.
    const NodeId copy = import_copy_id(note, digest);
    auto has_copy = txn.has_node(copy);
    if (!has_copy)
        return std::unexpected(from_graph(has_copy.error(), path));
    if (*has_copy) {
        ++outcome.unchanged;
        return {};
    }
    const std::int64_t at = to_ms(note.updated_at.value_or(std::chrono::system_clock::now()));
    NoteParts parts = split_note_path(path);
    std::string file = conflict_name(parts.name, parts.type, at);
    if (!parts.type.empty())
        file += "." + parts.type;
    auto copy_path = NotePath::parse(parts.folder.empty() ? file : parts.folder + "/" + file);
    if (!copy_path)
        return std::unexpected(copy_path.error());
    NOTEAPP_TRY(init_note(copy, *copy_path, note.bytes, at, at, kOriginUnknown));
    NOTEAPP_TRY_GRAPH(txn.set_property(copy, kMigratedSha, graph::Value{digest}), path);
    outcome.conflict_copies.push_back(*copy_path);
    return {};
}

std::expected<void, Error> WriteContext::import_one(const ImportedNote& note, ImportOutcome& outcome) {
    const std::string path = note.path.string();
    const NodeId id = import_id(note);
    auto exists = txn.has_node(id);
    if (!exists)
        return std::unexpected(from_graph(exists.error(), path));
    if (*exists)
        return import_existing(note, id, outcome);
    const std::int64_t updated = to_ms(note.updated_at.value_or(std::chrono::system_clock::now()));
    const std::int64_t created = note.created_at ? to_ms(*note.created_at) : updated;
    if (note.deleted_at) {
        // Çöpün yolu kTrashedFrom'dadır: düğüm kökte durur, canlı ağaçta boş klasör doğmaz; geri alma kurar.
        auto leaf = NotePath::parse(path.substr(path.rfind('/') + 1));
        if (!leaf)
            return std::unexpected(leaf.error());
        NOTEAPP_TRY(init_note(id, *leaf, note.bytes, created, updated, kOriginUnknown));
        NOTEAPP_TRY(mark_trashed(id, path, *note.deleted_at));
    } else {
        NOTEAPP_TRY(init_note(id, note.path, note.bytes, created, updated, kOriginUnknown));
    }
    NOTEAPP_TRY_GRAPH(txn.set_property(id, kMigratedSha, graph::Value{digest_of(note.bytes)}), path);
    ++outcome.imported;
    return {};
}

std::expected<ImportOutcome, Error> WriteContext::import_notes(std::span<const ImportedNote> notes) {
    ImportOutcome outcome;
    for (const ImportedNote& note : notes)
        if (auto one = import_one(note, outcome); !one)
            return std::unexpected(one.error());
    return outcome;
}

}  // namespace noteapp::core
