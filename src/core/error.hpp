#pragma once

#include <string>
#include <utility>
#include <vector>

namespace noteapp::core {

// Tüm modüllerin paylaştığı hata türü: makine-okunur kod + i18n anahtarı + argümanlar
// (ör. {"reason", "dot_dot"}, {"path", ...}). Kullanıcı metni burada değil, i18n kataloğunda.
struct Error {
    enum class Code {
        InvalidPath,
        NotFound,
        AlreadyExists,
        ModifiedExternally,
        NotEmpty,
        Io,
        ConfigInvalid,
        EditorNotFound,
        EditorFailed
    };

    Code code;
    std::string message_key;
    std::vector<std::pair<std::string, std::string>> args;
};

}  // namespace noteapp::core
