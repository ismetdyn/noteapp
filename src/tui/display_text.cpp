#include "tui/display_text.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <format>

namespace noteapp::tui {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

std::size_t sequence_length(unsigned char lead) {
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return lead >= 0xC0 ? 2 : 1;
}

// Sağdan sola geçersiz kılma ve benzerleri dosya adının görünen sırasını değiştirip uzantıyı gizleyebilir.
bool is_bidi_control(char32_t cp) {
    return cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069);
}

bool is_terminal_control(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

}  // namespace

std::string terminal_safe(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size();) {
        const std::size_t length = sequence_length(static_cast<unsigned char>(raw[at]));
        const std::string_view sequence = raw.substr(at, length);
        const auto decoded = core::decode_utf8(sequence);
        if (!decoded || decoded->size() != 1) {
            out += kReplacement;
            ++at;  // bozuk bayt tek tek: ardından gelen geçerli metin kaybolmasın
            continue;
        }
        const bool unsafe = is_terminal_control(decoded->front()) || is_bidi_control(decoded->front());
        if (unsafe)
            out += kReplacement;
        else
            out += sequence;
        at += sequence.size();
    }
    return out;
}

namespace {

std::tm local_tm(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local;
}

std::string format_tm(const std::tm& local, const char* format) {
    std::array<char, 32> text{};
    return std::string(text.data(), std::strftime(text.data(), text.size(), format, &local));
}

}  // namespace

std::string local_time_text(std::filesystem::file_time_type time) {
    return format_tm(local_tm(std::chrono::clock_cast<std::chrono::system_clock>(time)), "%Y-%m-%d %H:%M");
}

std::string short_time_text(std::filesystem::file_time_type time, std::chrono::system_clock::time_point now) {
    const std::tm local = local_tm(std::chrono::clock_cast<std::chrono::system_clock>(time));
    const std::tm today = local_tm(now);
    const bool same_day = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday;
    return format_tm(local, same_day ? "%H:%M" : "%Y-%m-%d");
}

std::string count_text(std::string_view key, std::size_t count) {
    const std::string full = std::string(key) + (count == 1 ? "_one" : "");
    return core::tr(full, {{"count", std::to_string(count)}});
}

std::string human_size(std::uintmax_t bytes) {
    constexpr double kStep = 1024.0;
    constexpr std::array kUnits = {"KiB", "MiB", "GiB", "TiB"};
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    auto value = static_cast<double>(bytes) / kStep;
    std::size_t unit = 0;
    while (value >= kStep && unit + 1 < kUnits.size()) {
        value /= kStep;
        ++unit;
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

}  // namespace noteapp::tui
