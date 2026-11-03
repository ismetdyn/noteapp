// `#etiket` kuralı ve (:Tag)/[:TAGGED] okumaları (SPEC-graph "Etiketler").
#include "core/note_store_impl.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <map>

namespace noteapp::core {

using graph::NodeId;

namespace {

constexpr std::size_t kMaxTagsPerNote = 256;  // işlem başına op sınırına düşmesin; fazlası etiketlenmez
constexpr std::size_t kMaxTagBytes = 128;
// Kodda `#include`, `#fff`, `# yorum` etiket değildir: yalnız düzyazı tipleri taranır.
constexpr std::array<std::string_view, 3> kProseTypes = {"md", "markdown", "txt"};

bool is_prose(std::string_view path) {
    const std::string type = split_note_path(path).type;
    return type.empty() || std::ranges::find(kProseTypes, type) != kProseTypes.end();
}

// `#`ten önce gelebilenler: satır başı, boşluk, ayraç. `a#b`, URL parçası (`/sayfa#bolum`), `\#`, `&#351;` ve
// Markdown çapa bağlantısı (`[bölüm](#bolum)`) böylece etiket olmaz.
bool opens_tag(std::string_view line, std::size_t at) {
    if (at == 0)
        return true;
    const char before = line[at - 1];
    if (before == '(')
        return at < 2 || line[at - 2] != ']';
    return before == ' ' || before == '\t' || before == ',' || before == ';';
}

bool is_ascii_tag_char(char32_t cp) {
    return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') || (cp >= U'0' && cp <= U'9') ||
           cp == U'_' || cp == U'-' || cp == U'/';
}

// ASCII dışında harf tablosu yok (yeni bağımlılık olmasın): kontrol, Latin-1 noktalaması, genel noktalama
// (sıfır genişlikli ve yön karakterleri dahil), ideografik boşluk ve BOM dışındaki her kod noktası harf
// sayılır. ponytail: Unicode kategorisi gerekiyorsa (ör. «», CJK noktalaması) bu liste büyür.
bool is_tag_char(char32_t cp) {
    if (cp < 0x80)
        return is_ascii_tag_char(cp);
    return !(cp <= 0xBF || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2000 && cp <= 0x206F) || cp == 0x3000 ||
             cp == 0xFEFF);
}

bool is_letter(char32_t cp) {
    return cp >= 0x80 || (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z');
}

// at'taki kod noktası ve uzunluğu; geçersiz UTF-8'de uzunluk 0.
std::pair<char32_t, std::size_t> code_point_at(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
    if (length == 0 || at + length > text.size())
        return {0, 0};
    const auto decoded = decode_utf8(text.substr(at, length));
    return decoded && decoded->size() == 1 ? std::pair{(*decoded)[0], length} : std::pair{char32_t{0}, 0UZ};
}

// `#`ten sonraki etiket metni; etiket değilse boş. end: taramanın sürdüğü konum.
std::string_view tag_at(std::string_view line, std::size_t hash, std::size_t& end) {
    std::size_t at = hash + 1;
    bool has_letter = false;
    while (at < line.size()) {
        const auto [cp, length] = code_point_at(line, at);
        if (length == 0 || !is_tag_char(cp))
            break;
        has_letter = has_letter || is_letter(cp);
        at += length;
    }
    end = std::max(at, hash + 1);
    std::string_view tag = line.substr(hash + 1, at - hash - 1);
    while (tag.ends_with('/'))  // `#proje/` iç içe ayıracıdır, etiketin parçası değil
        tag.remove_suffix(1);
    if (tag.empty() || tag.starts_with('/') || !has_letter || tag.size() > kMaxTagBytes)
        return {};
    return tag;
}

// Satır içi kod (`` `x` ``) ve [[bağlantı]] atlanır; kapanmayan işaret düz metindir.
std::size_t skip_span(std::string_view line, std::size_t at) {
    if (line[at] == '`') {
        std::size_t run = at;
        while (run < line.size() && line[run] == '`')
            ++run;
        // Kapanış aynı uzunlukta bir dizidir; daha uzun dizi içeriktir, arama onun sonundan sürer.
        const std::string_view fence = line.substr(at, run - at);
        for (std::size_t from = run;;) {
            const std::size_t close = line.find(fence, from);
            if (close == std::string_view::npos)
                return run;
            std::size_t end = close;
            while (end < line.size() && line[end] == '`')
                ++end;
            if (end - close == fence.size())
                return end;
            from = end;
        }
    }
    if (line.substr(at).starts_with("[[")) {
        const std::size_t close = line.find("]]", at + 2);
        return close == std::string_view::npos ? at + 2 : close + 2;
    }
    return at;
}

// Kuşatılmış kod bloğunun açılış ya da kapanış satırı: girintiden sonra en az üç ``` ya da ~~~. Girinti
// sınırsızdır: liste maddesinin altındaki çit dört boşluk ya da sekmeyle girer.
std::string_view fence_of(std::string_view line) {
    line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
    if (line.empty() || (line.front() != '`' && line.front() != '~'))
        return {};
    const std::size_t run = line.find_first_not_of(line.front());
    const std::size_t length = run == std::string_view::npos ? line.size() : run;
    return length >= 3 ? line.substr(0, length) : std::string_view{};
}

// Kapanış: aynı karakterden en az açılış kadar, ardında yalnız boşluk.
bool closes_fence(std::string_view line, std::string_view fence, std::string_view open) {
    if (fence.empty() || fence.front() != open.front() || fence.size() < open.size())
        return false;
    const std::size_t after = static_cast<std::size_t>(fence.data() + fence.size() - line.data());
    return line.find_first_not_of(" \t\r", after) == std::string_view::npos;
}

void scan_line(std::string_view line, std::map<std::string, std::string>& tags) {
    std::size_t at = 0;
    while (at < line.size() && tags.size() < kMaxTagsPerNote) {
        if (const std::size_t skipped = skip_span(line, at); skipped != at) {
            at = skipped;
            continue;
        }
        if (line[at] != '#' || !opens_tag(line, at)) {
            ++at;
            continue;
        }
        std::size_t end = at + 1;
        if (const std::string_view tag = tag_at(line, at, end); !tag.empty())
            tags.try_emplace(tag_key(tag), tag);
        at = end;
    }
}

}  // namespace

std::vector<std::string> note_tags(std::string_view path, std::string_view body) {
    if (!is_prose(path) || looks_binary(body))
        return {};
    std::map<std::string, std::string> tags;  // anahtar -> ilk yazım
    std::string_view open_fence;
    for (std::size_t start = 0; start <= body.size() && tags.size() < kMaxTagsPerNote;) {
        const std::size_t newline = std::min(body.find('\n', start), body.size());
        const std::string_view line = body.substr(start, newline - start);
        start = newline + 1;
        const std::string_view fence = fence_of(line);
        if (open_fence.empty() && !fence.empty())
            open_fence = fence;
        else if (open_fence.empty())
            scan_line(line, tags);
        else if (closes_fence(line, fence, open_fence))
            open_fence = {};
    }
    std::vector<std::string> out;
    out.reserve(tags.size());
    for (auto& [key, display] : tags)
        out.push_back(std::move(display));
    return out;
}

std::string tag_key(std::string_view tag) {
    auto decoded = decode_utf8(tag);
    if (!decoded)
        return std::string(tag);
    std::ranges::transform(*decoded, decoded->begin(), to_lower);
    return encode_utf8(*decoded);
}

std::expected<void, Error> WriteContext::update_tags(const NodeId& id, std::string_view path,
                                                     std::string_view body) {
    struct Wanted {
        std::string key;
        std::string display;
    };
    std::map<NodeId, Wanted> wanted;
    for (std::string& display : note_tags(path, body)) {
        std::string key = tag_key(display);
        const NodeId tag = graph::derive_node_id(kTagLabel, as_bytes(key));
        wanted.try_emplace(tag, Wanted{std::move(key), std::move(display)});
    }
    auto existing = neighbor_edges(id, graph::Direction::Out, kTagged, path);
    if (!existing)
        return std::unexpected(existing.error());
    std::vector<NodeId> dropped;
    for (const auto& [edge, tag] : *existing) {
        if (wanted.erase(tag) > 0)
            continue;
        NOTEAPP_TRY_GRAPH(txn.delete_edge(edge), path);
        dropped.push_back(tag);
    }
    for (const auto& [tag, names] : wanted) {
        NOTEAPP_TRY(ensure_tag(tag, names.key, names.display, path));
        NOTEAPP_TRY_GRAPH(txn.put_edge(graph::derive_edge_id(kTagged, as_bytes(id_text(id) + id_text(tag))),
                                       id, kTagged, tag),
                          path);
    }
    for (const NodeId& tag : dropped)
        NOTEAPP_TRY(remove_orphan_tag(tag, path));
    return {};
}

// Kimlik anahtardan türer: iki cihazın aynı etiketi tek düğümdür. Düğüm her kullanımda yeniden yazılır:
// başka cihazın onu görmeden yaptığı sahipsiz silme bu yazmayı görmemiştir, etkisiz kalır (SPEC-graph
// "Silme ve düzenleme çakışması"); yalnız kenar eklemek silinmiş düğümü canlandırmazdı.
std::expected<void, Error> WriteContext::ensure_tag(const NodeId& tag, const std::string& key,
                                                    const std::string& display, std::string_view path) {
    auto exists = txn.has_node(tag);
    if (!exists)
        return std::unexpected(from_graph(exists.error(), path));
    NOTEAPP_TRY_GRAPH(txn.put_node(tag), path);
    if (*exists)
        return {};
    NOTEAPP_TRY_GRAPH(txn.add_label(tag, kTagLabel), path);
    NOTEAPP_TRY_GRAPH(txn.set_property(tag, kName, graph::Value{key}), path);
    NOTEAPP_TRY_GRAPH(txn.set_property(tag, kTagDisplay, graph::Value{display}), path);
    return {};
}

// Hiç kenarı kalmayan etiket silinir. Kullanıcının Cypher'la kurduğu başka bir kenar varsa düğüm kalır.
std::expected<void, Error> WriteContext::remove_orphan_tag(const NodeId& tag, std::string_view path) {
    auto edges = neighbor_edges(tag, graph::Direction::Both, std::nullopt, path);
    if (!edges)
        return std::unexpected(edges.error());
    if (edges->empty())
        NOTEAPP_TRY_GRAPH(txn.delete_node(tag, false), path);
    return {};
}

std::expected<std::vector<std::pair<graph::EdgeId, NodeId>>, Error>
WriteContext::neighbor_edges(const NodeId& id, graph::Direction direction,
                             std::optional<std::string_view> type, std::string_view path) {
    auto cursor = txn.neighbors(id, direction, type);
    if (!cursor)
        return std::unexpected(from_graph(cursor.error(), path));
    std::vector<std::pair<graph::EdgeId, NodeId>> edges;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(from_graph(next.error(), path));
        if (!*next)
            return edges;
        edges.emplace_back((*next)->edge, (*next)->node);
    }
}

graph::Result<std::vector<std::string>> read_note_tags(graph::ReadTxn& txn, const NodeId& id) {
    auto cursor = txn.neighbors(id, graph::Direction::Out, kTagged);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::map<std::string, std::string> tags;  // anahtar -> ilk yazım; paralel kenar tek satır
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        auto name = txn.property((*next)->node, kName);
        auto display = txn.property((*next)->node, kTagDisplay);
        if (!name || !display)
            return std::unexpected(!name ? name.error() : display.error());
        const std::string* key = std::get_if<std::string>(&*name);
        const std::string* shown = std::get_if<std::string>(&*display);
        if (key != nullptr)
            tags.try_emplace(*key, shown != nullptr ? *shown : *key);
    }
    std::vector<std::string> out;
    out.reserve(tags.size());
    for (auto& [key, shown] : tags)
        out.push_back(std::move(shown));
    return out;
}

graph::Result<void> load_tags(graph::ReadTxn& txn, Snapshot& snapshot) {
    auto cursor = txn.nodes_with_label(kTagLabel);
    if (!cursor)
        return std::unexpected(cursor.error());
    std::vector<NodeId> ids;
    while (true) {
        auto next = cursor->next();
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        ids.push_back(**next);
    }
    for (const NodeId& tag : ids) {
        auto name = txn.property(tag, kName);
        auto display = txn.property(tag, kTagDisplay);
        auto notes = txn.neighbors(tag, graph::Direction::In, kTagged);
        if (!name || !display || !notes)
            return std::unexpected(!name ? name.error() : !display ? display.error() : notes.error());
        const std::string* key = std::get_if<std::string>(&*name);
        if (key == nullptr)
            continue;
        TagCount count{*key, *key, 0};
        if (const std::string* shown = std::get_if<std::string>(&*display))
            count.display = *shown;
        while (true) {
            auto next = notes->next();
            if (!next)
                return std::unexpected(next.error());
            if (!*next)
                break;
            const auto row = snapshot.live_by_id.find((*next)->node);
            if (row == snapshot.live_by_id.end())
                continue;  // çöpteki not etiketini tutar ama sayılmaz
            std::vector<std::string>& tags = snapshot.live[row->second].info.tags;
            if (std::ranges::find(tags, *key) == tags.end()) {
                tags.push_back(*key);
                ++count.count;
            }
        }
        if (count.count > 0)
            snapshot.tags.push_back(std::move(count));
    }
    std::ranges::sort(snapshot.tags, {}, &TagCount::name);
    for (NoteRow& row : snapshot.live)
        std::ranges::sort(row.info.tags);
    return {};
}

std::vector<TagCount> NoteStore::tags() const {
    const std::scoped_lock lock(impl_->mutex);
    const auto snapshot = impl_->current();
    return snapshot ? snapshot->tags : std::vector<TagCount>{};
}

}  // namespace noteapp::core
