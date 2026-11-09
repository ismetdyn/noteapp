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
        PartialCopy,  // çok işlemli kopya yarıda kaldı: yazılanlar yerinde, yeni adla yinelenmez
        InvalidQuery,     // görünümün Cypher sorgusu reddedildi (bozuk, yazan, not döndürmeyen)
        InvalidDocument,  // belge (PDF) okunamadı: bozuk, kesik ya da desteklenmeyen biçim
        Encrypted,        // belge parola korumalı; parola desteklenmez
        LimitExceeded,    // güvenilmez girdi bir [limits.*] sınırını aştı; iş yapılmadı
        Cancelled         // çağıran işi durdurdu (std::stop_token); sonuç yok, hata kullanıcıya gösterilmez
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
