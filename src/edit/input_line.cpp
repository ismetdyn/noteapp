#include "edit/input_line.hpp"

#include "core/text.hpp"
#include "edit/completion.hpp"
#include "search/fold.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr char kDelete = 0x7F;
constexpr std::string_view kSpaces = " \t\r\n";
// Bu uzunluktan itibaren boşluksuz harf-rakam karışımı jeton sayılır (`ghp_...`, JWT). Uzun bir not adı da
// buna takılabilir: yalnız geçmişe yazılmaz, güvenli tarafta yanılır.
constexpr std::size_t kMinTokenBytes = 20;

struct ArgumentKey {
    std::string_view name;
    std::array<std::string_view, 5> values;  // boşsa adaylar notlardan ya da yok
};

// Değerler parse_query'den geçer ("every offered argument value" testi): kayma testte düşer.
constexpr std::array<ArgumentKey, 10> kArguments = {{
    {"sort", {"name", "size", "updated", "created"}},
    {"type", {}},
    {"in", {}},
    {"size", {">1mb", "<1mb", ">100kb", "<100kb"}},
    {"updated", {"7d", "30d", "365d"}},
    {"created", {"7d", "30d", "365d"}},
    {"origin", {"windows", "linux", "macos", "android", "unknown"}},
    {"sync", {"synced", "modified", "new", "conflict_copy", "sync_off"}},
    {"tag", {}},
    {"view", {}},
}};

const ArgumentKey* argument_key(std::string_view name) {
    const std::string folded = search::fold_to_utf8(name);
    const auto* found = std::ranges::find(kArguments, std::string_view(folded), &ArgumentKey::name);
    return found == kArguments.end() ? nullptr : found;
}

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t previous_code_point(std::string_view text, std::size_t at) {
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && is_continuation(text[at]))
        --at;
    return at;
}

std::size_t next_code_point(std::string_view text, std::size_t at) {
    if (at >= text.size())
        return text.size();
    ++at;
    while (at < text.size() && is_continuation(text[at]))
        ++at;
    return at;
}

// Terminalden gelen kontrol baytı ve bozuk UTF-8 metne girmez.
bool is_typable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20 && text.front() != kDelete &&
           core::is_valid_utf8(text);
}

// Tek satırlık girdiye yapıştırma: satır sonu Enter gibi erken göndermesin. Sondaki satır sonları atılır,
// aradakiler ve diğer kontrol baytları (sekme dahil) boşluk olur.
std::string single_line(std::string_view pasted) {
    while (!pasted.empty() && (pasted.back() == '\n' || pasted.back() == '\r'))
        pasted.remove_suffix(1);
    std::string line(pasted);
    for (char& c : line)
        if (static_cast<unsigned char>(c) < 0x20 || c == kDelete)
            c = ' ';
    return line;
}

struct Range {
    std::size_t begin;
    std::size_t end;
};

// parse_query'nin sözcük kuralı: boşluk tırnak dışındaysa ayırır, kapanmamış tırnak sona dek sürer.
std::vector<Range> split_words(std::string_view text) {
    std::vector<Range> words;
    std::size_t at = 0;
    while (at < text.size()) {
        if (kSpaces.find(text[at]) != std::string_view::npos) {
            ++at;
            continue;
        }
        const std::size_t begin = at;
        bool quoted = false;
        for (; at < text.size() && (quoted || kSpaces.find(text[at]) == std::string_view::npos); ++at)
            if (text[at] == '"')
                quoted = !quoted;
        words.push_back({begin, at});
    }
    return words;
}

std::string without_quotes(std::string_view text) {
    std::string out;
    std::ranges::copy_if(text, std::back_inserter(out), [](char c) { return c != '"'; });
    return out;
}

// Search'te imlecin solundaki sözcük, öbür girdilerde imlece dek tüm metin.
Range word_before(InputKind kind, std::string_view text, std::size_t cursor) {
    if (kind != InputKind::Search)
        return {0, cursor};
    const std::vector<Range> words = split_words(text.substr(0, cursor));
    return !words.empty() && words.back().end == cursor ? words.back() : Range{cursor, cursor};
}

// Yolun son parçasındaki noktadan sonrası uzantıdır: tip adayı yalnız onu değiştirir.
std::optional<std::size_t> extension_start(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t from = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.rfind('.');
    return dot == std::string_view::npos || dot < from ? std::nullopt : std::optional(dot);
}

void append_all(std::vector<std::string>& out, const std::vector<std::string>& items) {
    out.insert(out.end(), items.begin(), items.end());
}

// `in:` `type:` `tag:` değerleri notlardan, `view:` kayıtlı görünümlerden, öbürleri tablodan.
std::vector<std::string> argument_values(const ArgumentKey& key, const InputSources* sources) {
    std::vector<std::string> values;
    const std::string prefix = std::string(key.name) + ":";
    for (const std::string_view value : key.values)
        if (!value.empty())
            values.push_back(prefix + std::string(value));
    if (!sources)
        return values;
    if (key.name == "in")
        for (const std::string& folder : sources->folders)
            values.push_back(prefix + folder);
    if (key.name == "type")
        for (const std::string& type : sources->types)
            values.push_back(prefix + type.substr(1));
    if (key.name == "tag")
        for (const std::string& tag : sources->tags)
            values.push_back(prefix + tag);
    if (key.name == "view")
        for (const std::string& view : sources->views)
            values.push_back(prefix + view);
    return values;
}

std::vector<std::string> search_candidates(std::string_view word, const InputSources* sources) {
    const std::size_t colon = word.find(':');
    if (colon != std::string_view::npos)
        if (const ArgumentKey* key = argument_key(word.substr(0, colon)))
            return argument_values(*key, sources);
    std::vector<std::string> out;
    out.reserve(kArguments.size());
    for (const ArgumentKey& key : kArguments)
        out.push_back(std::string(key.name) + ":");
    if (sources) {
        append_all(out, sources->folders);
        append_all(out, sources->names);
        append_all(out, sources->types);
    }
    return out;
}

// Boşluklu aday tek sözcük kalsın; anahtar tırnağın dışında olmalı, yoksa argüman sayılmaz.
std::string quoted_for_search(const std::string& candidate) {
    if (candidate.find(' ') == std::string::npos)
        return candidate;
    const std::size_t colon = candidate.find(':');
    const bool has_key = colon != std::string::npos && argument_key(candidate.substr(0, colon)) != nullptr;
    const std::size_t value = has_key ? colon + 1 : 0;
    return candidate.substr(0, value) + '"' + candidate.substr(value) + '"';
}

// `first_of` sırayla bakar: öncelikli listede aday varsa sonrakine geçilmez.
std::string remainder_in(const std::vector<std::string>& sorted, std::string_view prefix) {
    for (auto it = std::ranges::lower_bound(sorted, prefix); it != sorted.end() && it->starts_with(prefix);
         ++it)
        if (it->size() > prefix.size())
            return it->substr(prefix.size());
    return {};
}

bool contains_sorted(const std::vector<std::string>& sorted, std::string_view item) {
    const auto it = std::ranges::lower_bound(sorted, item);
    return it != sorted.end() && *it == item;
}

bool is_token_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.';
}

bool looks_like_token(std::string_view word) {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    return word.size() >= kMinTokenBytes && std::ranges::all_of(word, is_token_byte) &&
           std::ranges::any_of(word, is_digit) && !std::ranges::all_of(word, is_digit);
}

// core'un `IDENT=değer` / `IDENT: değer` kuralı `sort:name`i de atama sayar: bilinen argümanın anahtarı
// atılır, değeri aynı denetimden geçer (`in:TOKEN=abc`, `type:ghp_...` yine yazılmaz).
bool word_has_secret(std::string_view word) {
    const std::size_t colon = word.find(':');
    if (colon != std::string_view::npos && argument_key(word.substr(0, colon)) != nullptr)
        word.remove_prefix(colon + 1);
    return !core::assignment_line_key(word).empty() || looks_like_token(word) ||
           core::has_token_like_word(word);
}

// Satırda `anahtar: değer`, sözcükte `KEY=değer` ve jeton; bilinen argüman anahtarı istisna.
bool has_secret_value(std::string_view query) {
    const std::string_view key = core::secret_line_key(query);
    if (!key.empty() && argument_key(key) == nullptr)
        return true;
    return std::ranges::any_of(split_words(query), [query](const Range& range) {
        return word_has_secret(query.substr(range.begin, range.end - range.begin));
    });
}

bool has_note_name(std::string_view query, const InputSources& sources) {
    return std::ranges::any_of(split_words(query), [&](const Range& range) {
        const std::string word = without_quotes(query.substr(range.begin, range.end - range.begin));
        return contains_sorted(sources.names, word) || contains_sorted(sources.base_names, word);
    });
}

bool is_clean_text(std::string_view text) {
    return core::is_valid_utf8(text) && std::ranges::none_of(text, [](char c) {
               return static_cast<unsigned char>(c) < 0x20 || c == kDelete;
           });
}

bool is_storable(std::string_view query, std::size_t max_bytes) {
    return !query.empty() && query.size() <= max_bytes && is_clean_text(query) && !has_secret_value(query);
}

std::vector<std::string> sorted_unique(const std::set<std::string>& items) {
    return {items.begin(), items.end()};
}

}  // namespace

SharedInputSources build_input_sources(std::span<const core::NoteMeta> notes,
                                       std::span<const std::string> views) {
    std::set<std::string> folders;
    std::set<std::string> base_names;
    std::set<std::string> types;
    std::set<std::string> tags;
    auto sources = std::make_shared<InputSources>();
    sources->names.reserve(notes.size());
    for (const core::NoteMeta& note : notes) {
        const std::string path = note.path.string();
        if (!is_clean_text(path))
            continue;  // aday kabul edilince sorguya girer: kontrol baytı terminale ulaşmasın
        std::string_view base = path;
        for (std::size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1)) {
            folders.insert(path.substr(0, slash + 1));
            base = std::string_view(path).substr(slash + 1);
        }
        base_names.emplace(base);
        if (const std::size_t dot = base.rfind('.'); dot != std::string_view::npos && dot + 1 < base.size())
            types.emplace(base.substr(dot));
        sources->names.push_back(path);
        for (const std::string& tag : note.tags)
            if (is_clean_text(tag))
                tags.insert(tag);
    }
    std::ranges::sort(sources->names);
    sources->folders = sorted_unique(folders);
    sources->base_names = sorted_unique(base_names);
    sources->types = sorted_unique(types);
    sources->tags = sorted_unique(tags);
    // Görünüm adı senkronla gelebilir: aday kabul edilince sorguya girer, kontrol baytı terminale ulaşmasın.
    std::ranges::copy_if(views, std::back_inserter(sources->views), is_clean_text);
    return sources;
}

SharedInputSources word_input_sources(std::vector<std::string> words) {
    auto sources = std::make_shared<InputSources>();
    std::ranges::sort(words);
    sources->names = std::move(words);
    return sources;
}

std::vector<InputSpan> search_spans(std::string_view query) {
    std::vector<InputSpan> spans;
    for (const Range& range : split_words(query)) {
        const std::string_view word = query.substr(range.begin, range.end - range.begin);
        const auto parsed = search::parse_query(word);
        if (parsed && parsed->filters.empty() && !parsed->sort)
            continue;  // düz metin; bilinmeyen anahtar da buradan geçer
        const std::size_t value = range.begin + word.find(':') + 1;
        if (parsed) {
            spans.push_back({range.begin, value, InputSpan::Kind::Key});
            spans.push_back({value, range.end, InputSpan::Kind::Value});
            continue;
        }
        const std::size_t bad = range.begin + parsed.error().begin;
        if (bad >= value)
            spans.push_back({range.begin, value, InputSpan::Kind::Key});
        spans.push_back({bad, range.begin + parsed.error().end, InputSpan::Kind::Invalid});
    }
    return spans;
}

bool remember_search(std::vector<std::string>& history, std::string_view raw, const InputSources* sources,
                     const SearchHistoryLimits& limits) {
    const std::size_t first = raw.find_first_not_of(kSpaces);
    if (first == std::string_view::npos)
        return false;
    const std::string_view query = raw.substr(first, raw.find_last_not_of(kSpaces) - first + 1);
    if (!is_storable(query, limits.entry_bytes) || (sources && has_note_name(query, *sources)))
        return false;
    if (!history.empty() && history.front() == query)
        return false;
    std::erase(history, query);
    history.insert(history.begin(), std::string(query));
    if (history.size() > limits.entries)
        history.resize(limits.entries);
    return true;
}

std::string format_search_history(std::span<const std::string> history) {
    std::string out = "[";
    for (const std::string& entry : history) {
        if (out.size() > 1)
            out += ", ";
        out += '"';
        for (const char c : entry) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
    }
    return out + "]";
}

std::vector<std::string> parse_quoted_list(std::string_view value) {
    std::vector<std::string> items;
    std::size_t at = value.find('"');
    while (at != std::string_view::npos) {
        std::string entry;
        std::size_t i = at + 1;
        for (; i < value.size() && value[i] != '"'; ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '"' || value[i + 1] == '\\'))
                ++i;
            entry += value[i];
        }
        if (i >= value.size())
            break;  // kesik kayıt: dosya bozuk, kalanı okunmaz
        items.push_back(std::move(entry));
        at = value.find('"', i + 1);
    }
    return items;
}

std::vector<std::string> parse_search_history(std::string_view value, const SearchHistoryLimits& limits) {
    return kept_search_history(parse_quoted_list(value), limits);
}

std::vector<std::string> kept_search_history(std::vector<std::string> entries,
                                             const SearchHistoryLimits& limits) {
    std::vector<std::string> history;
    for (std::string& entry : entries)
        if (history.size() < limits.entries && is_storable(entry, limits.entry_bytes) &&
            std::ranges::find(history, entry) == history.end())
            history.push_back(std::move(entry));
    return history;
}

void InputLine::set_text(std::string text) {
    const std::size_t end = text.size();
    set_text(std::move(text), end);
}

void InputLine::set_text(std::string text, std::size_t cursor) {
    place(std::move(text), cursor);
    popup_.reset();
    history_.reset();
}

void InputLine::set_cursor(std::size_t cursor) {
    constexpr unsigned char kContinuationMask = 0xC0;
    constexpr unsigned char kContinuation = 0x80;
    cursor_ = std::min(cursor, text_.size());
    while (cursor_ > 0 && cursor_ < text_.size() &&
           (static_cast<unsigned char>(text_[cursor_]) & kContinuationMask) == kContinuation)
        --cursor_;
}

void InputLine::place(std::string text, std::size_t cursor) {
    text_ = std::move(text);
    cursor_ = std::min(cursor, text_.size());
}

InputLine::Result InputLine::handle(Key key, std::string_view typed, std::span<const std::string> recent) {
    if (popup_)
        return handle_popup(*popup_, key, typed);
    switch (key) {
    case Key::Char:
    case Key::Backspace:
    case Key::Delete:
        return edit(key, typed);
    case Key::Paste:
        return edit(Key::Char, single_line(typed));
    case Key::Left:
        cursor_ = previous_code_point(text_, cursor_);
        return Result::Consumed;
    case Key::Right:
        if (cursor_ == text_.size())
            return accept_ghost(recent);
        cursor_ = next_code_point(text_, cursor_);
        return Result::Consumed;
    case Key::Home:
        cursor_ = 0;
        return Result::Consumed;
    case Key::End:
        cursor_ = text_.size();
        return Result::Consumed;
    case Key::Tab:
        return open_popup();
    case Key::CtrlP:
    case Key::CtrlN:
        return kind_ == InputKind::Search ? step_history(recent, key == Key::CtrlP) : Result::Ignored;
    default:
        return Result::Ignored;
    }
}

InputLine::Result InputLine::edit(Key key, std::string_view typed) {
    if (key == Key::Char) {
        if (!is_typable(typed))
            return Result::Consumed;
        text_.insert(cursor_, typed);
        cursor_ += typed.size();
    } else {
        const std::size_t begin = key == Key::Backspace ? previous_code_point(text_, cursor_) : cursor_;
        const std::size_t end = key == Key::Backspace ? cursor_ : next_code_point(text_, cursor_);
        if (begin == end)
            return Result::Consumed;
        text_.erase(begin, end - begin);
        cursor_ = begin;
    }
    history_.reset();
    return Result::Edited;
}

InputLine::Result InputLine::accept_ghost(std::span<const std::string> recent) {
    const std::string rest = ghost(recent);
    if (rest.empty())
        return Result::Consumed;
    text_ += rest;
    cursor_ = text_.size();
    history_.reset();
    return Result::Edited;
}

std::string InputLine::ghost(std::span<const std::string> recent) const {
    if (popup_ || text_.empty() || cursor_ != text_.size())
        return {};
    if (kind_ == InputKind::Search) {
        for (const std::string& entry : recent)
            if (entry.size() > text_.size() && entry.starts_with(text_))
                return entry.substr(text_.size());
    }
    if (!sources_)
        return {};
    const Range word = word_before(kind_, text_, cursor_);
    const std::string_view prefix = std::string_view(text_).substr(word.begin, word.end - word.begin);
    if (prefix.empty())
        return {};
    if (kind_ == InputKind::Folder)
        return remainder_in(sources_->folders, prefix);
    std::string rest = kind_ == InputKind::Path ? remainder_in(sources_->folders, prefix) : std::string{};
    if (rest.empty())
        rest = remainder_in(sources_->names, prefix);
    return rest.empty() ? remainder_in(sources_->base_names, prefix) : rest;
}

// Yol girdisinde son parçada nokta varsa yalnız uzantı tamamlanır; yoksa klasörler ve adlar.
std::vector<std::string> InputLine::path_candidates(std::string& word, std::size_t& begin) const {
    std::vector<std::string> out;
    if (!sources_)
        return out;
    if (const auto dot = extension_start(word)) {
        begin += *dot;
        word.erase(0, *dot);
        return sources_->types;
    }
    append_all(out, sources_->folders);
    append_all(out, sources_->names);
    return out;
}

InputLine::Result InputLine::open_popup() {
    const Range range = word_before(kind_, text_, cursor_);
    std::string word = without_quotes(std::string_view(text_).substr(range.begin, range.end - range.begin));
    popup_begin_ = range.begin;
    if (kind_ == InputKind::Search)
        popup_candidates_ = search_candidates(word, sources_.get());
    else if (kind_ == InputKind::Path)
        popup_candidates_ = path_candidates(word, popup_begin_);
    else if (kind_ == InputKind::Folder)
        popup_candidates_ = sources_ ? sources_->folders : std::vector<std::string>{};
    else
        popup_candidates_ = sources_ ? sources_->names : std::vector<std::string>{};
    Popup& popup = popup_.emplace(Popup{std::move(word), {}, 0});
    refresh_popup(popup);
    if (popup.items.size() > 1)
        return Result::Consumed;
    const std::optional<std::string> only =
        popup.items.empty() ? std::nullopt : std::optional(popup.items.front());
    popup_.reset();
    return only && insert_candidate(*only) ? Result::Edited : Result::Consumed;
}

void InputLine::refresh_popup(Popup& popup) {
    popup.items = filter_candidates(popup_candidates_, popup.query, kPopupRows);
    popup.selected = std::min(popup.selected, popup.items.empty() ? 0 : popup.items.size() - 1);
}

bool InputLine::insert_candidate(const std::string& candidate) {
    const std::string inserted = kind_ == InputKind::Search ? quoted_for_search(candidate) : candidate;
    if (text_.compare(popup_begin_, cursor_ - popup_begin_, inserted) == 0)
        return false;
    text_.replace(popup_begin_, cursor_ - popup_begin_, inserted);
    cursor_ = popup_begin_ + inserted.size();
    history_.reset();
    return true;
}

InputLine::Result InputLine::handle_popup(Popup& popup, Key key, std::string_view typed) {
    switch (key) {
    case Key::Char:
        if (is_typable(typed)) {
            popup.query.append(typed);
            refresh_popup(popup);
        }
        return Result::Consumed;
    case Key::Backspace: {
        std::string& query = popup.query;
        query.erase(previous_code_point(query, query.size()));
        refresh_popup(popup);
        return Result::Consumed;
    }
    case Key::Up:
        popup.selected -= popup.selected > 0 ? 1 : 0;
        return Result::Consumed;
    case Key::Down:
        popup.selected += popup.selected + 1 < popup.items.size() ? 1 : 0;
        return Result::Consumed;
    case Key::Enter: {
        const std::optional<std::string> chosen =
            popup.items.empty() ? std::nullopt : std::optional(popup.items[popup.selected]);
        popup_.reset();
        return chosen && insert_candidate(*chosen) ? Result::Edited : Result::Consumed;
    }
    case Key::Escape:
        popup_.reset();
        return Result::Consumed;
    default:
        return Result::Consumed;
    }
}

InputLine::Result InputLine::step_history(std::span<const std::string> recent, bool older) {
    if (!history_)
        history_ = HistoryWalk{text_, std::nullopt};
    const std::string pattern = search::fold_to_utf8(history_->pattern);
    const auto matches = [&](std::size_t index) {
        return search::fold_to_utf8(recent[index]).find(pattern) != std::string::npos;
    };
    const auto show = [&](std::size_t index) {
        history_->index = index;
        place(recent[index], recent[index].size());
        return Result::Edited;
    };
    if (older) {
        for (std::size_t i = history_->index ? *history_->index + 1 : 0; i < recent.size(); ++i)
            if (matches(i))
                return show(i);
        return Result::Consumed;
    }
    if (!history_->index)
        return Result::Consumed;
    for (std::size_t i = *history_->index; i > 0; --i)
        if (matches(i - 1))
            return show(i - 1);
    history_->index.reset();
    place(history_->pattern, history_->pattern.size());
    return Result::Edited;
}

bool clears_input_error(Key key, InputLine::Result result) {
    return result == InputLine::Result::Edited ||
           (result == InputLine::Result::Consumed && (key == Key::Backspace || key == Key::Delete));
}

}  // namespace noteapp::tui
