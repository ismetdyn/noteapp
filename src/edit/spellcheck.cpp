#include "edit/spellcheck.hpp"

#include "core/text.hpp"
#include "edit/child_process.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kBannerPrefix = "@(#)";
constexpr std::size_t kMaxPending = 128;      // görünen satır sayısının çok üstünde; taşarsa en eskisi düşer
constexpr std::size_t kMaxCacheLines = 8192;  // üstünde silinir: görünen satırlar yeniden sorulur
// İşçi yanıt beklerken durma isteğini bu aralıkla yoklar: takılan çocuk çıkışı geciktirmesin.
constexpr auto kStopPoll = std::chrono::milliseconds(50);

// Bir yanıt satırı: `& sözcük sayı ofset: öneriler`, `? ...` ya da `# sözcük ofset`; diğerleri doğru sözcük.
std::string_view misspelled_word(std::string_view reply) {
    if (reply.size() < 3 || (reply[0] != '&' && reply[0] != '#' && reply[0] != '?') || reply[1] != ' ')
        return {};
    const std::string_view rest = reply.substr(2);
    return rest.substr(0, rest.find(' '));
}

// Ofset yerine sözcük satırda aranır: hunspell sürümleri ofseti bayt ya da karakter sayar, sıra ise kesindir.
std::vector<ByteRange> ranges_from_replies(std::string_view line, std::string_view replies) {
    std::vector<ByteRange> ranges;
    std::size_t from = 0;
    while (!replies.empty()) {
        const std::size_t end = replies.find('\n');
        const std::string_view word = misspelled_word(replies.substr(0, end));
        replies.remove_prefix(end == std::string_view::npos ? replies.size() : end + 1);
        if (word.empty())
            continue;
        const std::size_t at = line.find(word, from);
        if (at == std::string_view::npos)
            continue;
        ranges.push_back({at, at + word.size()});
        from = at + word.size();
    }
    return ranges;
}

class HunspellPipe {
public:
    // Banner gelmezse ya da süreç ölürse nullopt.
    static std::optional<HunspellPipe> start(const std::string& dictionary, std::chrono::milliseconds timeout,
                                             const std::stop_token& stop) {
        auto child = ChildProcess::start({"hunspell", "-a", "-i", "utf-8", "-d", dictionary}, true,
                                         {"LC_ALL=C.UTF-8"});
        if (!child)
            return std::nullopt;
        HunspellPipe pipe(std::move(*child), timeout);
        std::string banner;
        if (!pipe.read_until(banner, "\n", stop) || !banner.starts_with(kBannerPrefix))
            return std::nullopt;
        return pipe;
    }

    // `^`: satır ispell komutu sayılmasın (`*sözcük` kişisel sözlüğe yazardı). Yanıt boş satırla biter.
    std::optional<std::string> ask(std::string_view line, const std::stop_token& stop) {
        std::string request = "^";
        request += line;
        request += '\n';
        if (!child_.write_all(request, ChildProcess::Clock::now() + timeout_, stop))
            return std::nullopt;
        std::string replies;
        if (!read_until(replies, "\n\n", stop))
            return std::nullopt;
        return replies;
    }

private:
    HunspellPipe(ChildProcess child, std::chrono::milliseconds timeout)
        : child_(std::move(child)), timeout_(timeout) {}

    // `buffer_`dan `end` dizisine kadar olan kısmı `out`a alır. Yanıtı `*\n\n` ya da yalnız `\n` olabilir:
    // boş yanıt satır başındaki boş satırdır.
    bool read_until(std::string& out, std::string_view end, const std::stop_token& stop) {
        const auto deadline = ChildProcess::Clock::now() + timeout_;
        while (true) {
            if (end == "\n\n" && buffer_.starts_with('\n')) {
                buffer_.erase(0, 1);
                return true;
            }
            if (const std::size_t found = buffer_.find(end); found != std::string::npos) {
                out = buffer_.substr(0, found + 1);
                buffer_.erase(0, found + end.size());
                return true;
            }
            const auto now = ChildProcess::Clock::now();
            if (now >= deadline || stop.stop_requested())
                return false;
            const auto status = child_.read_some(buffer_, std::min(deadline, now + kStopPoll));
            if (status == ChildProcess::ReadStatus::Eof || status == ChildProcess::ReadStatus::Failed)
                return false;
            // Windows'ta hunspell stdout'u metin kipindedir (\r\n): yanıt sonu "\n\n" ancak \r atılınca görünür.
            std::erase(buffer_, '\r');
        }
    }

    ChildProcess child_;
    std::chrono::milliseconds timeout_;
    std::string buffer_;
};

}  // namespace

std::string dictionary_for_language(std::string_view language) {
    return language == "tr" ? "tr_TR" : "en_US";
}

bool spellcheck_applies_to(std::string_view type) {
    return type == "md" || type == "txt";
}

SpellChecker::SpellChecker(std::string dictionary, std::function<void()> on_ready,
                           std::chrono::milliseconds idle, std::chrono::milliseconds timeout,
                           std::size_t max_line_bytes)
    : dictionary_(std::move(dictionary)), on_ready_(std::move(on_ready)), idle_(idle), timeout_(timeout),
      max_line_bytes_(max_line_bytes),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

SpellChecker::~SpellChecker() = default;

std::vector<ByteRange> SpellChecker::misspelled(std::string_view line) {
    if (line.empty() || line.size() > max_line_bytes_ || line.find('\0') != std::string_view::npos ||
        !core::is_valid_utf8(line))
        return {};
    {
        const std::scoped_lock lock(mutex_);
        if (disabled_)
            return {};
        if (const auto found = cache_.find(std::string(line)); found != cache_.end())
            return found->second;
        if (cache_.size() >= kMaxCacheLines)
            cache_.clear();
        // Yanıt gelene dek boş sayılır: aynı satır her karede yeniden kuyruğa girmesin.
        cache_.emplace(line, std::vector<ByteRange>{});
        if (pending_.size() >= kMaxPending) {
            cache_.erase(pending_.front());
            pending_.erase(pending_.begin());
        }
        pending_.emplace_back(line);
        requested_at_ = std::chrono::steady_clock::now();
    }
    wake_.notify_all();
    return {};
}

bool SpellChecker::disabled() const {
    const std::scoped_lock lock(mutex_);
    return disabled_;
}

void SpellChecker::run(const std::stop_token& stop) {
    std::optional<HunspellPipe> pipe;  // işçiye ait: durunca çocuk öldürülür
    const auto disable = [this] {
        const std::scoped_lock lock(mutex_);
        disabled_ = true;
        pending_.clear();
        cache_.clear();
    };
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        wake_.wait(lock, stop, [this] { return !pending_.empty(); });
        if (stop.stop_requested())
            return;
        const auto ready_at = requested_at_ + idle_;
        if (std::chrono::steady_clock::now() < ready_at) {
            wake_.wait_until(lock, stop, ready_at, [] { return false; });
            continue;
        }
        const std::vector<std::string> lines = std::exchange(pending_, {});
        lock.unlock();
        if (!pipe)
            pipe = HunspellPipe::start(dictionary_, timeout_, stop);
        if (!pipe) {
            disable();
            return;
        }
        std::vector<std::pair<std::string, std::vector<ByteRange>>> answers;
        for (const std::string& line : lines) {
            const auto replies = pipe->ask(line, stop);
            if (!replies) {
                pipe.reset();
                disable();
                return;
            }
            answers.emplace_back(line, ranges_from_replies(line, *replies));
        }
        lock.lock();
        for (auto& [line, ranges] : answers)
            cache_.insert_or_assign(std::move(line), std::move(ranges));
        lock.unlock();
        on_ready_();
        lock.lock();
    }
}

}  // namespace noteapp::tui
