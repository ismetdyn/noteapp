#include "core/limits.hpp"

#include <array>
#include <charconv>
#include <span>
#include <utility>

namespace noteapp::core {

namespace {

using Suffix = std::pair<std::string_view, std::uint64_t>;

constexpr std::uint64_t kKibi = 1024;
constexpr std::uint64_t kKilo = 1000;
constexpr std::uint64_t kMillisPerSecond = 1000;
constexpr std::uint64_t kSecondsPerMinute = 60;
constexpr std::uint64_t kMinutesPerHour = 60;

constexpr std::array<Suffix, 10> kByteSuffixes = {{
    {"", 1},
    {"B", 1},
    {"KB", kKilo},
    {"MB", kKilo * kKilo},
    {"GB", kKilo * kKilo * kKilo},
    {"TB", kKilo * kKilo * kKilo * kKilo},
    {"KiB", kKibi},
    {"MiB", kKibi * kKibi},
    {"GiB", kKibi * kKibi * kKibi},
    {"TiB", kKibi * kKibi * kKibi * kKibi},
}};

constexpr std::array<Suffix, 5> kDurationSuffixes = {{
    {"", 1},
    {"ms", 1},
    {"s", kMillisPerSecond},
    {"min", kMillisPerSecond * kSecondsPerMinute},
    {"h", kMillisPerSecond * kSecondsPerMinute * kMinutesPerHour},
}};

constexpr std::array<Suffix, 1> kCountSuffixes = {{{"", 1}}};

std::span<const Suffix> suffixes(LimitUnit unit) {
    switch (unit) {
    case LimitUnit::Bytes:
        return kByteSuffixes;
    case LimitUnit::Milliseconds:
        return kDurationSuffixes;
    case LimitUnit::Count:
        break;
    }
    return kCountSuffixes;
}

std::optional<std::uint64_t> multiplier(LimitUnit unit, std::string_view suffix) {
    for (const auto& [name, factor] : suffixes(unit))
        if (name == suffix)
            return factor;
    return std::nullopt;
}

}  // namespace

std::optional<std::uint64_t> parse_limit(std::string_view text, LimitUnit unit) {
    std::uint64_t number = 0;
    const char* const end = text.data() + text.size();
    const auto [digits_end, error] = std::from_chars(text.data(), end, number);
    if (error != std::errc{})
        return std::nullopt;  // boş, işaretli, boşluklu ya da taşan sayı
    const std::string_view suffix(digits_end, static_cast<std::size_t>(end - digits_end));
    const std::optional<std::uint64_t> factor = multiplier(unit, suffix);
    // Üst sınır int64: değer dosyaya TOML tam sayısı olarak da yazılabilmeli.
    constexpr auto kMax = static_cast<std::uint64_t>(INT64_MAX);
    if (!factor || number > kMax / *factor)
        return std::nullopt;
    return number * *factor;
}

}  // namespace noteapp::core
