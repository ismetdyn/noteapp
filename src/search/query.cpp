#include "search/query.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <limits>
#include <numeric>
#include <utility>

namespace noteapp::search {

namespace {

namespace chr = std::chrono;
namespace fs = std::filesystem;

// Üst sınırlar file_clock'un (ns) taşmaması için: ±292 yıl; bu aralığın dışında not zamanı da olmaz.
constexpr int kMinYear = 1970;
constexpr int kMaxYear = 2200;
constexpr int kMaxAgeDays = 36500;
constexpr std::size_t kDateChars = 10;  // YYYY-MM-DD

enum class Key { Sort, Type, In, Size, Updated, Created, Origin, Sync };

// Geçersiz değerin gerekçesi: i18n "reason.<ad>".
using Reason = std::string_view;
constexpr Reason kMustBeSortField = "must_be_sort_field";
constexpr Reason kMustBeSizeBound = "must_be_size_bound";
constexpr Reason kMustBeAgeOrDate = "must_be_age_or_date";
constexpr Reason kMustBeSyncState = "must_be_sync_state";
constexpr Reason kMustBeFolder = "must_be_folder";
constexpr Reason kMustBeTypeName = "must_be_type_name";
constexpr Reason kMustNotBeEmpty = "must_not_be_empty";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

struct Token {
    std::size_t begin;
    std::size_t end;
};

// Sözcük, tırnak dışındaki boşlukta biter; kapanmamış tırnak sonuna kadar sürer (parse_terms kuralı).
std::vector<Token> tokenize(std::string_view query) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < query.size()) {
        if (is_space(query[i])) {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        bool is_quoted = false;
        for (; i < query.size() && (is_quoted || !is_space(query[i])); ++i) {
            if (query[i] == '"')
                is_quoted = !is_quoted;
        }
        tokens.push_back({begin, i});
    }
    return tokens;
}

std::optional<Key> known_key(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, Key>, 8> kKeys{{{"sort", Key::Sort},
                                                                            {"type", Key::Type},
                                                                            {"in", Key::In},
                                                                            {"size", Key::Size},
                                                                            {"updated", Key::Updated},
                                                                            {"created", Key::Created},
                                                                            {"origin", Key::Origin},
                                                                            {"sync", Key::Sync}}};
    const std::string folded = fold_to_utf8(name);
    const auto* found =
        std::ranges::find(kKeys, std::string_view(folded), &std::pair<std::string_view, Key>::first);
    return found == kKeys.end() ? std::nullopt : std::optional(found->second);
}

template <typename Number> std::optional<Number> parse_number(std::string_view digits) {
    Number value{};
    const char* end = digits.data() + digits.size();
    const auto [ptr, ec] = std::from_chars(digits.data(), end, value);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

std::expected<SortSpec, Reason> parse_sort(std::string_view value) {
    SortSpec sort{SortField::Name, value.starts_with('-')};
    if (sort.descending)
        value.remove_prefix(1);
    if (value == "name")
        sort.field = SortField::Name;
    else if (value == "size")
        sort.field = SortField::Size;
    else if (value == "updated")
        sort.field = SortField::Updated;
    else if (value == "created")
        sort.field = SortField::Created;
    else
        return std::unexpected(kMustBeSortField);
    return sort;
}

// ">" ya da "<" öneki; yoksa nullopt.
std::optional<Filter::Compare> take_comparison(std::string_view& value) {
    if (value.empty() || (value.front() != '>' && value.front() != '<'))
        return std::nullopt;
    const auto compare = value.front() == '>' ? Filter::Compare::Greater : Filter::Compare::Less;
    value.remove_prefix(1);
    return compare;
}

std::optional<std::uintmax_t> unit_bytes(std::string_view unit) {
    constexpr std::uintmax_t kKilo = 1024;
    if (unit.empty() || unit == "b")
        return 1;
    if (unit == "kb")
        return kKilo;
    if (unit == "mb")
        return kKilo * kKilo;
    if (unit == "gb")
        return kKilo * kKilo * kKilo;
    return std::nullopt;
}

std::expected<Filter, Reason> parse_size(std::string_view value) {
    Filter filter{Filter::Kind::Size};
    const auto compare = take_comparison(value);
    if (!compare)
        return std::unexpected(kMustBeSizeBound);
    filter.compare = *compare;
    const auto unit_start = std::ranges::find_if_not(value, [](char c) { return c >= '0' && c <= '9'; });
    const auto digits = value.substr(0, static_cast<std::size_t>(unit_start - value.begin()));
    const auto count = parse_number<std::uintmax_t>(digits);
    const auto unit = unit_bytes(value.substr(digits.size()));
    if (!count || !unit || *count > std::numeric_limits<std::uintmax_t>::max() / *unit)
        return std::unexpected(kMustBeSizeBound);
    filter.bytes = *count * *unit;
    return filter;
}

std::optional<chr::sys_days> parse_date(std::string_view text) {
    if (text.size() != kDateChars || text[4] != '-' || text[7] != '-')
        return std::nullopt;
    const auto year = parse_number<int>(text.substr(0, 4));
    const auto month = parse_number<unsigned>(text.substr(5, 2));
    const auto day = parse_number<unsigned>(text.substr(8, 2));
    if (!year || !month || !day || *year < kMinYear || *year > kMaxYear)
        return std::nullopt;
    const chr::year_month_day date{chr::year(*year), chr::month(*month), chr::day(*day)};
    return date.ok() ? std::optional(chr::sys_days(date)) : std::nullopt;
}

std::expected<Filter, Reason> parse_time(Filter::Kind kind, std::string_view value) {
    Filter filter{kind};
    if (const auto compare = take_comparison(value)) {
        const auto day = parse_date(value);
        if (!day)
            return std::unexpected(kMustBeAgeOrDate);
        filter.compare = *compare;
        filter.day = *day;
        return filter;
    }
    if (!value.ends_with('d'))
        return std::unexpected(kMustBeAgeOrDate);
    const auto days = parse_number<int>(value.substr(0, value.size() - 1));
    if (!days || *days < 1 || *days > kMaxAgeDays)
        return std::unexpected(kMustBeAgeOrDate);
    filter.compare = Filter::Compare::Within;
    filter.age = chr::days(*days);
    return filter;
}

std::expected<Filter, Reason> parse_sync(std::string_view value) {
    static constexpr std::array<std::pair<std::string_view, core::SyncState>, 5> kStates{
        {{"synced", core::SyncState::Synced},
         {"modified", core::SyncState::Modified},
         {"new", core::SyncState::New},
         {"conflict_copy", core::SyncState::ConflictCopy},
         {"sync_off", core::SyncState::SyncOff}}};
    const auto* found =
        std::ranges::find(kStates, value, &std::pair<std::string_view, core::SyncState>::first);
    if (found == kStates.end())
        return std::unexpected(kMustBeSyncState);
    Filter filter{Filter::Kind::Sync};
    filter.sync = found->second;
    return filter;
}

std::expected<Filter, Reason> parse_type(std::string_view value) {
    if (value.find_first_of("./") != std::string_view::npos)
        return std::unexpected(kMustBeTypeName);
    Filter filter{Filter::Kind::Type};
    filter.text = value;
    return filter;
}

// Klasör adı katlanmadan NotePath'ten geçer: gizli ad (.trash) ve ".." aramaya kapı olmasın.
std::expected<Filter, Reason> parse_folder(std::string_view raw) {
    if (raw.ends_with('/'))
        raw.remove_suffix(1);
    if (!core::NotePath::parse(raw))
        return std::unexpected(kMustBeFolder);
    Filter filter{Filter::Kind::Folder};
    filter.text = fold_to_utf8(raw) + "/";
    return filter;
}

std::expected<Filter, Reason> parse_filter(Key key, std::string_view raw, std::string_view folded) {
    switch (key) {
    case Key::Type:
        return parse_type(folded);
    case Key::In:
        return parse_folder(raw);
    case Key::Size:
        return parse_size(folded);
    case Key::Updated:
        return parse_time(Filter::Kind::Updated, folded);
    case Key::Created:
        return parse_time(Filter::Kind::Created, folded);
    case Key::Sync:
        return parse_sync(folded);
    case Key::Origin:
    case Key::Sort:
        break;
    }
    Filter filter{Filter::Kind::Origin};
    filter.text = folded;
    return filter;
}

std::expected<void, Reason> add_argument(Key key, std::string_view value, ParsedQuery& out) {
    const std::string folded = fold_to_utf8(value);
    if (key == Key::Sort) {
        const auto sort = parse_sort(folded);
        if (!sort)
            return std::unexpected(sort.error());
        out.sort = *sort;
        return {};
    }
    auto filter = parse_filter(key, value, folded);
    if (!filter)
        return std::unexpected(filter.error());
    out.filters.push_back(std::move(*filter));
    return {};
}

std::string without_quotes(std::string_view text) {
    std::string result;
    std::ranges::copy_if(text, std::back_inserter(result), [](char c) { return c != '"'; });
    return result;
}

QueryError query_error(Token range, std::string_view query, std::string_view key, Reason reason) {
    return {range.begin, range.end,
            core::Error{core::Error::Code::ConfigInvalid,
                        "error.query_invalid",
                        {{"key", std::string(key)},
                         {"value", std::string(query.substr(range.begin, range.end - range.begin))},
                         {"reason", std::string(reason)}}}};
}

fs::file_time_type local_midnight(chr::sys_days day) {
    const chr::year_month_day date{day};
    std::tm tm{};
    tm.tm_year = static_cast<int>(date.year()) - 1900;
    tm.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
    tm.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
    tm.tm_isdst = -1;
    return chr::file_clock::from_sys(chr::system_clock::from_time_t(std::mktime(&tm)));
}

// Zaman filtresinin eşiği sorgu başına bir kez hesaplanır. ">gün" günün bittiği an, "<gün" başladığı an.
fs::file_time_type time_bound(const Filter& filter, chr::system_clock::time_point now) {
    switch (filter.compare) {
    case Filter::Compare::Within:
        return chr::file_clock::from_sys(now - filter.age);
    case Filter::Compare::Greater:
        return local_midnight(filter.day + chr::days(1));
    case Filter::Compare::Less:
        break;
    }
    return local_midnight(filter.day);
}

bool matches_time(const std::optional<fs::file_time_type>& value, Filter::Compare compare,
                  fs::file_time_type bound) {
    if (!value)
        return false;
    return compare == Filter::Compare::Less ? *value < bound : *value >= bound;
}

std::string_view file_name(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool matches_type(const std::string& path, const std::string& type) {
    const std::string_view name = file_name(path);
    const auto dot = name.rfind('.');
    return dot != std::string_view::npos && fold_to_utf8(name.substr(dot + 1)) == type;
}

bool matches(const Filter& filter, const core::NoteMeta& note, fs::file_time_type bound) {
    switch (filter.kind) {
    case Filter::Kind::Type:
        return matches_type(note.path.string(), filter.text);
    case Filter::Kind::Folder:
        return fold_to_utf8(note.path.string()).starts_with(filter.text);
    case Filter::Kind::Size:
        return filter.compare == Filter::Compare::Less ? note.size < filter.bytes : note.size > filter.bytes;
    case Filter::Kind::Updated:
        return matches_time(note.updated_at, filter.compare, bound);
    case Filter::Kind::Created:
        return matches_time(note.created_at, filter.compare, bound);
    case Filter::Kind::Origin:
        return note.origin && fold_to_utf8(*note.origin) == filter.text;
    case Filter::Kind::Sync:
        break;
    }
    return note.sync_status == filter.sync;
}

template <typename Iterator, typename KeyOf>
void stable_sort_by(Iterator first, Iterator last, bool descending, const KeyOf& key_of) {
    std::stable_sort(first, last, [&](std::size_t a, std::size_t b) {
        return descending ? key_of(b) < key_of(a) : key_of(a) < key_of(b);
    });
}

}  // namespace

std::expected<ParsedQuery, QueryError> parse_query(std::string_view query) {
    ParsedQuery out;
    for (const Token token : tokenize(query)) {
        const std::string_view word = query.substr(token.begin, token.end - token.begin);
        const auto colon = word.find(':');
        const auto key = colon == std::string_view::npos ? std::nullopt : known_key(word.substr(0, colon));
        if (!key) {
            if (!out.text.empty())
                out.text += ' ';
            out.text += word;
            continue;
        }
        const std::string_view key_name = word.substr(0, colon);
        const std::string value = without_quotes(word.substr(colon + 1));
        if (value.empty())
            return std::unexpected(query_error(token, query, key_name, kMustNotBeEmpty));
        if (auto added = add_argument(*key, value, out); !added)
            return std::unexpected(
                query_error({token.begin + colon + 1, token.end}, query, key_name, added.error()));
    }
    return out;
}

void sort_notes(std::vector<std::size_t>& indices, const SortSpec& sort,
                std::span<const core::NoteMeta> notes) {
    const auto first = indices.begin();
    const auto last = indices.end();
    switch (sort.field) {
    case SortField::Name: {
        std::vector<std::string> names(notes.size());
        for (const std::size_t i : indices)
            names[i] = fold_to_utf8(file_name(notes[i].path.string()));
        stable_sort_by(first, last, sort.descending,
                       [&](std::size_t i) -> const std::string& { return names[i]; });
        break;
    }
    case SortField::Size:
        stable_sort_by(first, last, sort.descending, [&](std::size_t i) { return notes[i].size; });
        break;
    case SortField::Updated:
        stable_sort_by(first, last, sort.descending, [&](std::size_t i) { return notes[i].updated_at; });
        break;
    case SortField::Created: {
        const auto known_end = std::stable_partition(
            first, last, [&](std::size_t i) { return notes[i].created_at.has_value(); });
        stable_sort_by(first, known_end, sort.descending,
                       [&](std::size_t i) { return *notes[i].created_at; });
        break;
    }
    }
}

std::vector<std::size_t> apply(std::span<const Filter> filters, const std::optional<SortSpec>& sort,
                               std::span<const core::NoteMeta> notes, chr::system_clock::time_point now) {
    std::vector<fs::file_time_type> bounds;
    bounds.reserve(filters.size());
    for (const Filter& filter : filters) {
        const bool is_time = filter.kind == Filter::Kind::Updated || filter.kind == Filter::Kind::Created;
        bounds.push_back(is_time ? time_bound(filter, now) : fs::file_time_type{});
    }

    std::vector<std::size_t> indices;
    indices.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        bool passes = true;
        for (std::size_t f = 0; passes && f < filters.size(); ++f)
            passes = matches(filters[f], notes[i], bounds[f]);
        if (passes)
            indices.push_back(i);
    }
    if (sort)
        sort_notes(indices, *sort, notes);
    return indices;
}

}  // namespace noteapp::search
