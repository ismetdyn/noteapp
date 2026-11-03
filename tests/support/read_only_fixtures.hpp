#pragma once

// Salt-okunur kuralının ortak örnekleri (app::classify): TUI ve GUI testleri aynı baytlarla sınar, iki
// arayüzde kural ayrışırsa ikisinden biri düşer.
#include "app/document.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace noteapp::test {

struct ReadOnlyCase {
    const char* name;  // not adı (`nul` Windows ayrılmış adıdır, NotePath reddeder)
    std::string bytes;
    app::ReadOnly reason;
};

// `limit` arayüzün max_note_bytes değeridir: büyük örnek ondan bir bayt uzundur. NUL, ikili koklamanın
// (ilk 8 KB) ötesindedir: looks_binary görmez, ayrı kural yakalar.
inline std::vector<ReadOnlyCase> read_only_cases(std::size_t limit) {
    constexpr std::size_t kPastSniffWindow = 9000;
    return {
        {"late-nul.txt", std::string(kPastSniffWindow, 'a') + std::string("\0tail", 5),
         app::ReadOnly::EmbeddedNul},
        {"latin.txt", "caf\xE9 bozuk \xFF\xFE", app::ReadOnly::InvalidUtf8},
        {"big.txt", std::string(limit + 1, 'x'), app::ReadOnly::TooLarge},
    };
}

}  // namespace noteapp::test
