#pragma once

// LSP 3.17 protokol katmanı: çerçeveleme, istemci mesajları ve sunucu mesajı ayrıştırıcıları; süreç ve UI
// yok. Sunucu güvenilmezdir: boyut ve sayı sınırları burada uygulanır. Metinler ham kalır, ekrana basmadan
// önce temizlik UI katmanındadır.
#include "core/json_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core::lsp {

inline constexpr std::size_t kMaxHeaderBytes = 4U * 1024U;  // başlık bloğu, son "\r\n\r\n" dahil
inline constexpr std::size_t kMaxBodyBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxDiagnostics = 1000;
inline constexpr std::size_t kMaxDiagnosticMessageBytes = 512;
// Popup'ta gösterilebilecekten çok fazlası; sınır ayırmayı ve sıralama süresini tutar.
inline constexpr std::size_t kMaxCompletionItems = 500;
// 16 MiB'lik gövde sınırsız değer sayısıyla GiB'larca Json düğümü ayırırdı; büyük bir completion listesi
// (5000 öğe × ~20 değer) bunun altındadır. Derinlik LSPAny `data` alanlarına yer bırakır.
inline constexpr JsonLimits kMessageLimits{.max_depth = 64, .max_values = 250000, .max_bytes = kMaxBodyBytes};

enum class FrameError {
    HeaderTooLarge,
    BodyTooLarge,
    MissingContentLength,
    BadContentLength,
    MalformedHeader
};

// Sunucunun stdout'undan gelen baytları biriktirir, tam gövdeleri sırayla verir. Hata kalıcıdır: akışın
// hizası kaybolmuştur, çağıran sunucuyu kapatır.
class FrameReader {
public:
    void append(std::string_view bytes);
    // Tam bir gövde varsa onu, henüz yoksa nullopt.
    std::expected<std::optional<std::string>, FrameError> next();

private:
    std::string buffer_;
    // Okunmuş önek her çerçevede silinmez (çok küçük çerçevede O(n²) kaydırma); append yarıyı aşınca
    // sıkıştırır.
    std::size_t consumed_ = 0;
    std::optional<FrameError> error_;
};

// "Content-Length: N\r\n\r\n" + gövde.
std::string frame(std::string_view body);
// Gövdeyi kMessageLimits ile ayrıştırır.
std::optional<Json> parse_message(std::string_view body);

struct Position {
    std::uint32_t line = 0;
    std::uint32_t character = 0;  // birimi anlaşılan PositionEncoding'dir
};
struct Range {
    Position start;
    Position end;
};

enum class PositionEncoding { Utf8, Utf16 };
// initialize cevabının `result`ı; alan yoksa ya da tanınmıyorsa LSP varsayılanı UTF-16.
PositionEncoding negotiated_encoding(const Json& initialize_result);

// İstemci mesajlarının gövdeleri; frame() ile gönderilir. Metinler JSON'a geçerli UTF-8 olarak yazılır.
// `initialization_options` hazır JSON metnidir; boşsa alan hiç yazılmaz.
std::string initialize_request(std::int64_t id, std::int64_t process_id, std::string_view root_uri,
                               std::string_view initialization_options = {});
std::string initialized_notification();
// `settings` hazır JSON metni.
std::string did_change_configuration_notification(std::string_view settings);
// params alanı olmayan bildirim (yaml/registerContentRequest).
std::string bare_notification(std::string_view method);
std::string did_open_notification(std::string_view uri, std::string_view language_id, std::int64_t version,
                                  std::string_view text);
// Tam eşitleme: değişiklik tüm metindir.
std::string did_change_notification(std::string_view uri, std::int64_t version, std::string_view text);
std::string did_close_notification(std::string_view uri);
std::string completion_request(std::int64_t id, std::string_view uri, Position position);
std::string shutdown_request(std::int64_t id);
std::string exit_notification();
// Sunucudan gelen istekse (method + id) cevabı, değilse nullopt. `workspace/configuration`ın her öğesi
// `settings`ten noktalı bölüm yoluyla cevaplanır ("http.proxy" -> settings.http.proxy; bölümsüz öğe ayarların
// tamamı; olmayan yol null). Gerisi -32601. Cevap `max_bytes`ı aşınca kurulması kesilir ve geçersiz, sınırı
// aşan bir metin döner: çağıran onu göndermeden sunucuyu durdurmalıdır (öğe yağdıran sunucu GiB ayırtmasın).
std::optional<std::string> reply_to_server_request(const Json& message,
                                                   const Json& settings = Json{.kind = Json::Kind::Null},
                                                   std::size_t max_bytes = kMaxBodyBytes);

enum class DiagnosticSeverity { Unspecified = 0, Error = 1, Warning = 2, Information = 3, Hint = 4 };
struct Diagnostic {
    Range range;
    DiagnosticSeverity severity = DiagnosticSeverity::Unspecified;
    std::string message;  // tek satır, en çok kMaxDiagnosticMessageBytes; ham
};
struct PublishedDiagnostics {
    std::string uri;
    std::optional<std::int64_t> version;
    std::vector<Diagnostic> diagnostics;  // en çok kMaxDiagnostics; aralığı ya da mesajı bozuk olan atlanır
};
// `textDocument/publishDiagnostics` bildiriminin `params`ı; uri ya da dizi yoksa nullopt.
std::optional<PublishedDiagnostics> parse_publish_diagnostics(const Json& params);

struct CompletionItem {
    std::string label;
    std::string insert_text;  // textEdit.newText, yoksa insertText, yoksa label
    std::uint32_t kind = 0;   // CompletionItemKind; 0 = yok
};
// `textDocument/completion` cevabının `result`ı: CompletionItem[], CompletionList ya da null. En çok
// kMaxCompletionItems; etiketsiz öğe atlanır.
std::vector<CompletionItem> parse_completion_result(const Json& result);

// Satır içi sütun dönüşümü. Konum bir karakterin ortasına düşerse o karakterin başına, satır sonunu aşarsa
// satır sonuna iner (LSP 3.17 Position). Geçersiz UTF-8 baytı bir birim sayılır.
std::size_t utf16_to_byte_column(std::string_view line, std::uint32_t utf16_column);
std::uint32_t byte_to_utf16_column(std::string_view line, std::size_t byte_column);

// Mutlak yolun `file://` URI'si: `/`, `:` ve RFC 3986 unreserved dışı her bayt %XX. Windows'ta
// `file:///C:/...`.
std::string file_uri(const std::filesystem::path& absolute_path);

}  // namespace noteapp::core::lsp
