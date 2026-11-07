#include "app/map_query.hpp"

#include "core/i18n.hpp"
#include "core/note_props.hpp"
#include "core/note_views.hpp"
#include "core/text.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace noteapp::app {

namespace {

using Element = core::QueryElement;
constexpr std::size_t kShortIdChars = 8;  // düğüm kimliğinin tabloda gösterilen başı (32 onaltılıktan)

std::string_view trimmed(std::string_view text) {
    constexpr std::string_view kSpaces = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kSpaces);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(kSpaces) - first + 1);
}

// Bir kez, sonuç kurulurken: önce `max_bytes`ta kesilir (dev gövdenin tamamı temizlenmez), kod noktası
// ortasındaysa geri çekilir; sonra temizlenir. Bozuk baytın U+FFFD'si kesimden sonra eklendiğinden metin sınırı
// en çok üç katına dek aşabilir (yaklaşık sınır).
std::string shown_text(std::string_view raw, std::size_t max_bytes) {
    constexpr unsigned char kContinuationMask = 0xC0;
    constexpr unsigned char kContinuation = 0x80;
    if (raw.size() <= max_bytes)
        return core::terminal_safe(raw);
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(raw[cut]) & kContinuationMask) == kContinuation)
        --cut;
    return core::terminal_safe(raw.substr(0, cut)) + "…";
}

std::string real_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return code == std::errc{} ? std::string(buffer.data(), end) : std::string();
}

// UTC, milisaniyeli: "2026-11-06 12:00:00.000".
std::string time_text(std::int64_t unix_ms) {
    return core::timestamp_text(unix_ms);  // uç değer (senkron) taşmadan ham yazılır
}

// Sonucu tabloya çevirirken vurguyu da toplar: tabloda görünen her not, etiket ve kenar haritada yanar.
class OutcomeBuilder {
public:
    explicit OutcomeBuilder(const core::UserQuery& query) : query_(query) {}

    QueryCell cell(const graph::QueryValue& value) {
        return std::visit(
            [&](const auto& v) -> QueryCell {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return {QueryCell::Kind::Null, "null"};
                else if constexpr (std::is_same_v<T, bool>)
                    return {QueryCell::Kind::Value, v ? "true" : "false"};
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return {QueryCell::Kind::Value, std::to_string(v)};
                else if constexpr (std::is_same_v<T, double>)
                    return {QueryCell::Kind::Value, real_text(v)};
                else if constexpr (std::is_same_v<T, std::string>)
                    return {QueryCell::Kind::Value, v};
                else if constexpr (std::is_same_v<T, graph::Bytes>)
                    return {QueryCell::Kind::Value, "<bytes:" + std::to_string(v.data.size()) + ">"};
                else if constexpr (std::is_same_v<T, graph::Timestamp>)
                    return {QueryCell::Kind::Value, time_text(v.unix_ms)};
                else if constexpr (std::is_same_v<T, graph::NodeId>)
                    return node(v);
                else if constexpr (std::is_same_v<T, graph::EdgeId>)
                    return edge(v);
                else
                    return list(v);
            },
            value.v);
    }

    MapHighlight take_highlight() { return std::move(highlight_); }

private:
    QueryCell node(const graph::NodeId& id) {
        const auto found = query_.nodes.find(id);
        if (found == query_.nodes.end())
            return {QueryCell::Kind::Node, "?"};
        const Element& element = found->second;
        switch (element.kind) {
        case Element::Kind::Note:
            highlight_.notes.insert(element.key);
            return {QueryCell::Kind::Note, element.key, element.key};
        case Element::Kind::TrashedNote:
            return {QueryCell::Kind::Node, core::tr("map.query_trashed", {{"path", element.key}})};
        case Element::Kind::Tag:
            highlight_.tags.insert(element.key);
            return {QueryCell::Kind::Tag, "#" + element.key};
        default: {
            const std::string id_text = element.key.substr(0, kShortIdChars);
            return {QueryCell::Kind::Node,
                    element.name.empty() ? "(" + id_text + ")" : "(:" + element.name + " " + id_text + ")"};
        }
        }
    }

    QueryCell edge(const graph::EdgeId& id) {
        const auto found = query_.edges.find(id);
        if (found == query_.edges.end())
            return {QueryCell::Kind::Edge, "?"};
        const Element& element = found->second;
        std::string middle = "-[" + element.key + "]->";
        std::string from = element.from;
        std::string to = element.to;
        // Kenar uçlarıyla görünür: döndürülen ilişkinin notları da yanar ve kapsama girer.
        switch (element.kind) {
        case Element::Kind::Link:
            highlight_.links.emplace(element.from, element.to);
            highlight_.notes.insert({element.from, element.to});
            break;
        case Element::Kind::Relation:
            highlight_.relations.insert(core::RelationId{id.bytes});
            highlight_.notes.insert({element.from, element.to});
            middle = "-[" + (element.name.empty() ? element.key : element.name) + (element.directed ? "]->" : "]-");
            if (!element.directed && to < from)  // yönsüzde uçlar kimlik sırasındadır; haritadaki gibi yol sırası
                std::swap(from, to);
            break;
        case Element::Kind::Tagged:
            highlight_.tagged.emplace(element.from, element.to);
            highlight_.notes.insert(element.from);
            highlight_.tags.insert(element.to);
            to = "#" + element.to;
            break;
        default:
            if (element.from.empty())  // silinmiş ya da önizlemede kurulmuş: yalnız kimlik
                return {QueryCell::Kind::Edge, "[" + element.key.substr(0, kShortIdChars) + "]"};
            break;
        }
        return {QueryCell::Kind::Edge, from + " " + middle + " " + to};
    }

    QueryCell list(const graph::QueryList& items) {
        std::string text = "[";
        for (std::size_t i = 0; i < items.items.size(); ++i)
            text += (i == 0 ? "" : ", ") + cell(items.items[i]).text;
        return {QueryCell::Kind::List, text + "]"};
    }

    const core::UserQuery& query_;
    MapHighlight highlight_;
};

QueryTable raw_table(const core::UserQuery& query, OutcomeBuilder& builder) {
    QueryTable table;
    table.columns = query.result.columns;
    table.rows.reserve(query.result.rows.size());
    for (const auto& row : query.result.rows) {
        std::vector<QueryCell> cells;
        cells.reserve(row.size());
        for (const graph::QueryValue& value : row)
            cells.push_back(builder.cell(value));
        table.rows.push_back(std::move(cells));
    }
    return table;
}

}  // namespace

std::string cell_text(std::string_view raw, std::size_t max_bytes) {
    return shown_text(raw, max_bytes);
}

QueryTable query_table(const core::UserQuery& query) {
    OutcomeBuilder builder(query);
    return raw_table(query, builder);
}

QueryOutcome query_outcome(const core::UserQuery& query, bool wrote, std::size_t cell_bytes, std::string_view view) {
    QueryOutcome outcome;
    OutcomeBuilder builder(query);
    outcome.table = raw_table(query, builder);
    for (std::string& column : outcome.table.columns)
        column = shown_text(column, cell_bytes);
    for (auto& row : outcome.table.rows)
        for (QueryCell& cell : row)
            cell.text = shown_text(cell.text, cell_bytes);  // `note` ham kalır: not onunla açılır
    outcome.highlight = builder.take_highlight();
    outcome.changes = query.changes;
    outcome.wrote = wrote;
    if (query.naming_error)  // yazıldı ama sonuç adlanamadı: tablo eksik olabilir, yazma yine raporlanır
        outcome.summary = core::tr("map.query_written_unnamed", {{"changes", query_changes_text(query.changes)},
                                                                  {"error", core::format_error(*query.naming_error)}});
    else if (wrote)
        outcome.summary = core::tr("map.query_written", {{"changes", query_changes_text(query.changes)}});
    else if (!view.empty())  // görünüm yazılan metni gölgeleyebilir: hangisinin çalıştığı görünsün
        outcome.summary = core::tr("map.query_view", {{"name", std::string(view)},
                                                      {"rows", std::to_string(query.result.rows.size())},
                                                      {"notes", std::to_string(outcome.highlight.notes.size())}});
    else
        outcome.summary = core::tr("map.query_read", {{"rows", std::to_string(query.result.rows.size())},
                                                      {"notes", std::to_string(outcome.highlight.notes.size())}});
    return outcome;
}

std::string query_changes_text(const core::QueryChanges& changes) {
    const std::pair<const char*, std::size_t> counters[] = {
        {"map.changes_nodes_created", changes.nodes_created},
        {"map.changes_nodes_deleted", changes.nodes_deleted},
        {"map.changes_relationships_created", changes.relationships_created},
        {"map.changes_relationships_deleted", changes.relationships_deleted},
        {"map.changes_properties_set", changes.properties_set},
        {"map.changes_labels_added", changes.labels_added},
        {"map.changes_labels_removed", changes.labels_removed},
    };
    std::string text;
    for (const auto& [key, count] : counters) {
        if (count == 0)
            continue;
        if (!text.empty())
            text += ", ";
        text += core::tr(key, {{"count", std::to_string(count)}});
    }
    return text.empty() ? core::tr("map.changes_none") : text;
}

// Sınıflandırma ayrıştırmadır, depo okunmaz. Yazma önizlenir: korunan veri ve sorgunun kendi hatası onay
// sorulmadan görünür.
MapQuery::Step MapQuery::run(core::NoteStore& store, std::string_view text) {
    pending_.reset();
    // Kayıtlı görünümün adı önce çözülür (TUI Tab tamamlaması; GUI de): ad senkronla gelebilir ve Cypher gibi
    // okunabilir (`MATCH (n) DETACH DELETE n`); görünüm her zaman salt-okunur ve görünüm profiliyle çalışır,
    // adı kullanıcının yazması ya da limits.query'li okuması olmaz (inceleme, Sev-2). Arama kırpılmış metinledir;
    // saklı ad kenar boşluğu taşıyamaz (validate_view_name, kayıt ve okuma): ikisi aynı biçimde eşleşir.
    // Yalnız NotFound "görünüm değil" demektir: başka hata (Busy, G/Ç) gösterilir; metin görünümün adı
    // olabilirdi, Cypher'a düşüp yazma olarak çalışmasın.
    if (const std::string_view name = trimmed(text); !name.empty()) {
        const auto view = core::find_view(store, name);
        if (view)
            return run_view(store, view->name);  // özet kayıtlı adı söyler
        if (view.error().code != core::Error::Code::NotFound)
            return fail(view.error());
    }
    const auto writes = core::query_writes(text, limits_);
    if (!writes)
        return fail(writes.error());
    if (!*writes) {
        const Step step = finish(core::run_user_query(store, text, core::QueryAction::Read, limits_), false);
        if (step == Step::Ran)
            read_query_ = std::string(text);
        return step;
    }
    const auto preview = core::run_user_query(store, text, core::QueryAction::Preview, limits_);
    if (!preview)
        return fail(preview.error());
    pending_ = std::string(text);
    confirm_text_ = core::tr("map.query_confirm", {{"changes", query_changes_text(preview->changes)}});
    error_.clear();
    return Step::Confirm;
}

MapQuery::Step MapQuery::run_view(const core::NoteStore& store, std::string_view name) {
    pending_.reset();
    const Step step = finish(core::run_saved_view(store, name), false, name);
    if (step == Step::Ran)
        read_query_.clear();  // sonuç görünümündür: kaydedilecek yazılı sorgu yok
    return step;
}

MapQuery::Step MapQuery::apply(core::NoteStore& store) {
    if (!pending_)
        return Step::Failed;
    const std::string text = *std::exchange(pending_, std::nullopt);
    const Step step = finish(core::run_user_query(store, text, core::QueryAction::Write, limits_), true);
    if (step == Step::Ran)
        read_query_.clear();  // yazan sorgu görünüm olamaz
    return step;
}

void MapQuery::clear() {
    pending_.reset();
    confirm_text_.clear();
    outcome_.reset();
    error_.clear();
    read_query_.clear();
    table_ = false;
}

std::expected<void, core::Error> MapQuery::save_view(core::NoteStore& store, std::string_view name) const {
    if (read_query_.empty())
        return std::unexpected(core::Error{core::Error::Code::InvalidQuery, "error.view_needs_query", {}});
    return core::save_view(store, name, read_query_, false);
}

MapQuery::Step MapQuery::fail(const core::Error& error) {
    error_ = core::format_error(error);
    return Step::Failed;
}

MapQuery::Step MapQuery::finish(std::expected<core::UserQuery, core::Error> done, bool wrote, std::string_view view) {
    if (!done)
        return fail(done.error());
    outcome_ = query_outcome(*done, wrote, cell_bytes_, view);
    error_.clear();
    return Step::Ran;
}

}  // namespace noteapp::app
