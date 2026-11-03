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
        EditorFailed,
        MigrationRequired,
        PartialCopy,  // çok işlemli kopya yarıda kaldı: yazılanlar yerinde, yeni adla yinelenmez
        InvalidQuery  // görünümün Cypher sorgusu reddedildi (bozuk, yazan, not döndürmeyen)
    };

    Code code{};
    std::string message_key;
    std::vector<std::pair<std::string, std::string>> args;
};

}  // namespace noteapp::core

// expected döndüren çağrının hatasını aynen yayar: çok adımlı yazma ve senkron akışı okunur kalsın.
#define NOTEAPP_TRY(expr)                                                                                    \
    do {                                                                                                     \
        if (auto noteapp_try_result = (expr); !noteapp_try_result)                                           \
            return std::unexpected(noteapp_try_result.error());                                              \
    } while (false)
