#include "tui/completion.hpp"

#include "core/note_store.hpp"
#include "core/text.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <unordered_set>
#include <utility>

namespace noteapp::tui {

namespace {

// ponytail: öneri imleç çevresindeki pencereden gelir; daha uzaktaki sözcükler 1 MiB üstü notta görünmez. Ama
// tuş başına süre not boyutundan bağımsızdır (10 MiB'te de bütçe içinde). Gerekirse artımlı sözcük tablosu.
constexpr std::size_t kNoteWindowBytes = 256 * 1024;
constexpr std::size_t kMinPrefixBytes = 2;
constexpr std::size_t kMinTableWordBytes = 3;
constexpr std::size_t kMaxWordBytes = 64;
constexpr std::size_t kMaxTableWords = 200'000;
constexpr std::size_t kTableRangeScan = 256;  // önek aralığında en sık geçeni ararken bakılan en çok sözcük
constexpr std::size_t kMaxCandidates = 5'000;
constexpr std::size_t kMaxNoteWords = 2'000;
constexpr std::uintmax_t kMaxTableNoteBytes = 2 * 1024 * 1024;
constexpr std::uintmax_t kMaxTableTotalBytes = 64 * 1024 * 1024;
// Gölge öneri her adayda satırını arar: uzun değer satırında tuş süresi sınırlı kalsın.
constexpr int kMaxRejectedCandidates = 64;

// Çok baytlı karakterlerin bütün baytları sözcüktür: öneri kod noktası ortasında bitmez.
bool is_word_byte(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

std::size_t word_end(std::string_view text, std::size_t from) {
    const std::size_t limit = std::min(text.size(), from + kMaxWordBytes + 1);
    std::size_t at = from;
    while (at < limit && is_word_byte(text[at]))
        ++at;
    return at;
}

struct Window {
    std::size_t begin;
    std::size_t end;
};

Window window_around(std::size_t size, std::size_t cursor) {
    return {cursor > kNoteWindowBytes ? cursor - kNoteWindowBytes : 0,
            std::min(size, cursor + kNoteWindowBytes)};
}

// Betik ve yapılandırma tiplerinde değerler de sır taşır: öneriye yalnız satır anahtarları girer.
bool keys_only_type(std::string_view type) {
    constexpr std::array<std::string_view, 15> kTypes = {"sh",   "bash", "zsh",  "fish",  "ps1",
                                                         "bat",  "cmd",  "json", "jsonc", "json5",
                                                         "conf", "cfg",  "tf",   "xml",   "dockerfile"};
    return core::type_has_secret_values(type) || std::ranges::find(kTypes, type) != kTypes.end();
}

// Satırın öneriye girebilen kısmı: sır taşıyan tipte anahtar, diğerinde `KEY=değer` satırının anahtarı, öbür
// satırlarda satırın tamamı. Belirteç gibi sözcük taşıyan kısım hiç girmez: parçası da öneride görünmesin.
std::string_view offerable_part(std::string_view line, bool keys_only) {
    std::string_view part = core::secret_line_key(line);
    if (!keys_only) {
        const std::string_view key = core::assignment_line_key(line);
        part = key.empty() ? line : key;
    }
    return core::has_token_like_word(part) ? std::string_view{} : part;
}

// Bu konumdaki sözcük satırın önerilebilir kısmında mı?
bool in_offerable_part(std::string_view text, std::size_t at, std::size_t length, bool keys_only,
                       const Window& window) {
    const std::string_view before = text.substr(window.begin, at - window.begin);
    const std::size_t newline = before.rfind('\n');
    if (newline == std::string_view::npos && window.begin > 0)
        return false;  // satır başı pencerenin dışında: karar verilemez, önerilmez
    const std::size_t start = newline == std::string_view::npos ? window.begin : window.begin + newline + 1;
    const std::size_t stop = text.substr(at, window.end - at).find('\n');
    const std::size_t line_end = stop == std::string_view::npos ? window.end : at + stop;
    const std::string_view part = offerable_part(text.substr(start, line_end - start), keys_only);
    if (part.empty())
        return false;
    const auto begin = static_cast<std::size_t>(part.data() - text.data());
    return at >= begin && at + length <= begin + part.size();
}

// `at`taki önekle başlayan, ondan uzun sözcüğün kalanı; uygun değilse boş.
std::optional<std::string> completion_at(std::string_view text, std::size_t at, std::string_view prefix,
                                         std::size_t cursor, bool keys_only, const Window& window) {
    if ((at > 0 && is_word_byte(text[at - 1])) || at + prefix.size() == cursor)
        return std::nullopt;
    const std::size_t end = word_end(text, at + prefix.size());
    if (end == at + prefix.size() || end - at > kMaxWordBytes)
        return std::nullopt;
    if (!in_offerable_part(text, at, end - at, keys_only, window))
        return std::nullopt;
    return std::string(text.substr(at + prefix.size(), end - at - prefix.size()));
}

std::optional<std::string> suggest_before(std::string_view text, std::string_view prefix, std::size_t cursor,
                                          bool keys_only, const Window& window) {
    std::size_t searched = cursor - prefix.size();
    for (int rejected = 0; searched > window.begin && rejected < kMaxRejectedCandidates; ++rejected) {
        const std::string_view part =
            text.substr(window.begin, std::min(searched - 1 + prefix.size(), text.size()) - window.begin);
        const std::size_t found = part.rfind(prefix);
        if (found == std::string_view::npos)
            return std::nullopt;
        searched = window.begin + found;
        if (auto rest = completion_at(text, searched, prefix, cursor, keys_only, window))
            return rest;
    }
    return std::nullopt;
}

std::optional<std::string> suggest_after(std::string_view text, std::string_view prefix, std::size_t cursor,
                                         bool keys_only, const Window& window) {
    std::size_t from = cursor;
    for (int rejected = 0; from < window.end && rejected < kMaxRejectedCandidates; ++rejected) {
        const std::size_t found = text.substr(from, window.end - from).find(prefix);
        if (found == std::string_view::npos)
            return std::nullopt;
        const std::size_t at = from + found;
        if (auto rest = completion_at(text, at, prefix, cursor, keys_only, window))
            return rest;
        from = at + 1;
    }
    return std::nullopt;
}

std::optional<std::string> suggest_from_table(const WordTable& table, std::string_view prefix) {
    const auto first = std::ranges::lower_bound(table.words, prefix);
    auto index = static_cast<std::size_t>(first - table.words.begin());
    std::optional<std::size_t> best;
    for (std::size_t scanned = 0; index < table.words.size() && scanned < kTableRangeScan;
         ++index, ++scanned) {
        const std::string& word = table.words[index];
        if (!word.starts_with(prefix))
            break;
        if (word.size() > prefix.size() && (!best || table.counts[index] > table.counts[*best]))
            best = index;
    }
    if (!best)
        return std::nullopt;
    return table.words[*best].substr(prefix.size());
}

template <typename Add> void for_each_word(std::string_view text, const Add& add) {
    std::size_t at = 0;
    while (at < text.size()) {
        if (!is_word_byte(text[at])) {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < text.size() && is_word_byte(text[end]))
            ++end;
        add(text.substr(at, end - at));
        at = end;
    }
}

// Değer ve (sır taşıyan tipte) yorum hiç görülmez.
template <typename Add> void for_each_offerable_word(std::string_view text, bool keys_only, const Add& add) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        for_each_word(offerable_part(text.substr(0, end), keys_only), add);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
}

// Pencerenin kestiği baş ve son satır atılır: anahtar mı değer mi olduğu görülemez.
std::string_view whole_lines(std::string_view text, const Window& window) {
    std::string_view part = text.substr(window.begin, window.end - window.begin);
    if (window.end < text.size()) {
        const std::size_t last = part.rfind('\n');
        part = last == std::string_view::npos ? std::string_view{} : part.substr(0, last);
    }
    if (window.begin > 0) {
        const std::size_t first = part.find('\n');
        part = first == std::string_view::npos ? std::string_view{} : part.substr(first + 1);
    }
    return part;
}

}  // namespace

WordTable build_word_table(const std::vector<NoteText>& notes) {
    std::map<std::string, std::uint32_t, std::less<>> counted;
    const auto add = [&](std::string_view word) {
        if (word.size() < kMinTableWordBytes || word.size() > kMaxWordBytes)
            return;
        const auto found = counted.find(word);
        if (found != counted.end())
            ++found->second;
        else if (counted.size() < kMaxTableWords)
            counted.emplace(word, 1U);
    };
    for (const NoteText& note : notes)
        for_each_offerable_word(note.text, keys_only_type(note.type), add);
    WordTable table;
    table.words.reserve(counted.size());
    table.counts.reserve(counted.size());
    for (auto& [word, count] : counted) {
        table.words.push_back(word);
        table.counts.push_back(count);
    }
    return table;
}

std::string_view word_prefix_at(std::string_view text, std::size_t cursor) {
    if (cursor > text.size() || (cursor < text.size() && is_word_byte(text[cursor])))
        return {};
    std::size_t start = cursor;
    while (start > 0 && cursor - start <= kMaxWordBytes && is_word_byte(text[start - 1]))
        --start;
    return cursor - start > kMaxWordBytes ? std::string_view{} : text.substr(start, cursor - start);
}

std::optional<std::string> suggest_completion(std::string_view text, std::size_t cursor,
                                              std::string_view type, const WordTable* table) {
    const std::string_view prefix = word_prefix_at(text, cursor);
    if (prefix.size() < kMinPrefixBytes)
        return std::nullopt;
    const bool keys_only = keys_only_type(type);
    const Window window = window_around(text.size(), cursor);
    if (auto rest = suggest_before(text, prefix, cursor, keys_only, window))
        return rest;
    if (auto rest = suggest_after(text, prefix, cursor, keys_only, window))
        return rest;
    return table ? suggest_from_table(*table, prefix) : std::nullopt;
}

std::vector<std::string> completion_candidates(std::string_view text, std::size_t cursor,
                                               std::string_view type, const WordTable* table) {
    const std::string_view typing = word_prefix_at(text, cursor);
    std::vector<std::string> candidates;
    std::unordered_set<std::string_view> seen;
    const auto add = [&](std::string_view word, std::size_t limit) {
        if (word.size() <= kMaxWordBytes && word != typing && candidates.size() < limit &&
            seen.insert(word).second)
            candidates.emplace_back(word);
    };
    const Window window = window_around(text.size(), cursor);
    const auto add_note_word = [&](std::string_view word) { add(word, kMaxNoteWords); };
    for_each_offerable_word(whole_lines(text, window), keys_only_type(type), add_note_word);
    if (!table)
        return candidates;
    // Önekle başlayanlar önce: tablo sınırı aşsa da yazılana en yakınlar kalır.
    auto first = std::ranges::lower_bound(table->words, typing);
    for (; first != table->words.end() && first->starts_with(typing); ++first)
        add(*first, kMaxCandidates);
    for (const std::string& word : table->words)
        add(word, kMaxCandidates);
    return candidates;
}

std::vector<std::string> filter_candidates(const std::vector<std::string>& candidates, std::string_view query,
                                           std::size_t limit) {
    if (query.empty())
        return {candidates.begin(),
                candidates.begin() + static_cast<std::ptrdiff_t>(std::min(limit, candidates.size()))};
    std::vector<std::pair<double, const std::string*>> matched;
    for (const std::string& candidate : candidates)
        if (const auto match = search::fuzzy_match(query, candidate))
            matched.emplace_back(match->score, &candidate);
    std::ranges::stable_sort(matched, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> shown;
    for (std::size_t i = 0; i < matched.size() && i < limit; ++i)
        shown.push_back(*matched[i].second);
    return shown;
}

WordTableBuilder::WordTableBuilder(std::chrono::milliseconds idle)
    : idle_(idle), worker_([this](const std::stop_token& stop) { run(stop); }) {}

WordTableBuilder::~WordTableBuilder() = default;

void WordTableBuilder::set_store(core::NoteStore store) {
    const std::scoped_lock lock(mutex_);
    store_ = std::move(store);
}

void WordTableBuilder::enable() {
    {
        const std::scoped_lock lock(mutex_);
        if (enabled_)
            return;
        enabled_ = true;
        pending_ = true;
    }
    wake_.notify_all();
}

void WordTableBuilder::request() {
    {
        const std::scoped_lock lock(mutex_);
        pending_ = true;
        requested_at_ = std::chrono::steady_clock::now();
    }
    wake_.notify_all();
}

std::shared_ptr<const WordTable> WordTableBuilder::latest() const {
    const std::scoped_lock lock(mutex_);
    return table_;
}

void WordTableBuilder::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        wake_.wait(lock, stop, [this] { return enabled_ && pending_; });
        if (stop.stop_requested())
            return;
        const auto ready_at = requested_at_ + idle_;
        if (std::chrono::steady_clock::now() < ready_at) {
            wake_.wait_until(lock, stop, ready_at, [] { return false; });
            continue;
        }
        pending_ = false;
        if (!store_)
            continue;
        const core::NoteStore store = *store_;
        lock.unlock();
        std::vector<NoteText> notes;
        std::uintmax_t total = 0;
        for (const core::NoteInfo& info : store.list()) {
            if (stop.stop_requested())
                return;
            if (info.size > kMaxTableNoteBytes || total + info.size > kMaxTableTotalBytes)
                continue;
            auto content = store.read(info.path);
            if (!content || core::looks_binary(content->text) || !core::is_valid_utf8(content->text))
                continue;
            total += info.size;
            const std::u8string extension = info.path.value().extension().u8string();
            notes.push_back(
                {extension.empty() ? std::string{} : std::string(extension.begin() + 1, extension.end()),
                 std::move(content->text)});
        }
        auto table = std::make_shared<const WordTable>(build_word_table(notes));
        lock.lock();
        table_ = std::move(table);
    }
}

}  // namespace noteapp::tui
