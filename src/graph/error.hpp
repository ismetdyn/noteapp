#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::graph {

// core::Error ile aynı şekil: makine-okunur kod + i18n anahtarı + argümanlar. Motor projenin başka bir
// modülüne bağımlı olmadığından tür ayrıdır; uygulama katmanı sınırda çevirir. API istisna fırlatmaz.
struct Error {
    enum class Code {
        InvalidArgument,
        NotFound,
        Busy,
        Corrupt,
        TooNew,
        Syntax,
        Unsupported,
        ReadOnlyViolation,
        LimitExceeded,
        BadFrame,
        Io
    };

    Code code;
    std::string message_key;
    std::vector<std::pair<std::string, std::string>> args;
};

template <class T> using Result = std::expected<T, Error>;

}  // namespace noteapp::graph
