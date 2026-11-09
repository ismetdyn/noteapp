#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// PDF işçisi (`note __pdf-worker`) ile istemci (pdfview::PdfDocument) arasındaki ikili çerçeveler. Küçük-sonlu,
// sabit genişlikli alanlar; sıkı istek/cevap sırası. İşçi güvenilmez PDF'i ayrıştırır: istemci her cevabı burada
// tanımlanan katı denetimlerden geçirir, uymayanı bozuk çerçeve sayar (işçi öldürülür).
namespace noteapp::pdfview::protocol {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 16;     // {u32 kind, u32 reserved = 0, u64 length}
inline constexpr std::size_t kOpenFieldBytes = 44;  // {u32 version, u64 parent_pid, u64 x4 sınır}
inline constexpr std::size_t kFailedFieldBytes = 20;  // {u32 reason, u64 a, u64 b}
inline constexpr std::size_t kMaxFailedDetail = 256;
inline constexpr std::size_t kRenderedFieldBytes = 8;  // {u32 width, u32 height}

enum class Kind : std::uint32_t {
    Open = 1,
    Text = 2,
    Render = 3,
    Opened = 0x81,
    TextResult = 0x82,
    Rendered = 0x83,
    Failed = 0xFF,
};

// İşçinin iş hatası; istemci core::Error'u kendi değerleriyle kurar (işçi i18n anahtarı ya da metin göndermez).
enum class FailReason : std::uint32_t {
    Invalid = 1,         // bozuk belge ya da sayfa
    Encrypted = 2,       // parola ister
    TooManyPages = 3,    // a = sayfa sayısı
    RenderTooLarge = 4,  // a = genişlik, b = yükseklik
    TextTooLarge = 5,    // a = metin baytı
    OutOfMemory = 6,     // bellek bütçesi (limits.pdf.context_memory)
    NoMemory = 7,        // a = genişlik, b = yükseklik (piksel tamponu)
};

struct Header {
    Kind kind{};
    std::uint64_t length = 0;
};

struct OpenFields {
    std::uint32_t version = kVersion;
    std::uint64_t parent_pid = 0;
    std::uint64_t pages = 0;  // [limits.pdf] değerleri; 0 = sınırsız
    std::uint64_t render_pixels = 0;
    std::uint64_t text_bytes = 0;
    std::uint64_t context_memory = 0;
};

struct Failure {
    FailReason reason = FailReason::Invalid;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::string detail;  // yalnız log için; istemci terminal_safe'ten geçirir
};

// Küçük-sonlu yazma/okuma (memcpy'siz, platformdan bağımsız).
void put_u32(std::string& out, std::uint32_t value);
void put_u64(std::string& out, std::uint64_t value);
std::uint32_t get_u32(std::string_view bytes, std::size_t offset);  // ön koşul: offset + 4 <= size
std::uint64_t get_u64(std::string_view bytes, std::size_t offset);  // ön koşul: offset + 8 <= size

std::string encode_header(Kind kind, std::uint64_t length);
// Tam 16 bayt değilse, ayrılmış alan 0 değilse ya da tür bilinmiyorsa nullopt.
std::optional<Header> decode_header(std::string_view bytes);

// Open çerçevesinin başlığı ve alanları; `pdf_size` baytlık PDF ayrı yazılır (büyük belge kopyalanmaz).
std::string encode_open_head(const OpenFields& fields, std::uint64_t pdf_size);
// Yükün ilk kOpenFieldBytes'ı; kısa yük ya da farklı sürümde nullopt. PDF baytları alanlardan sonra gelir.
std::optional<OpenFields> decode_open(std::string_view payload);

std::string encode_text_request(std::uint32_t page);
std::string encode_render_request(std::uint32_t page, float scale);
std::optional<std::uint32_t> decode_text_request(std::string_view payload);  // uzunluk tam 4 değilse nullopt
struct RenderRequest {
    std::uint32_t page = 0;
    float scale = 0.0F;
};
std::optional<RenderRequest> decode_render_request(std::string_view payload);  // uzunluk tam 8

std::string encode_opened(std::uint32_t page_count);
std::string encode_text_result(std::string_view text);
std::string encode_rendered(std::uint32_t width, std::uint32_t height, std::string_view rgba);
std::string encode_failed(const Failure& failure);  // detail kMaxFailedDetail'e kesilir
// Uzunluk [kFailedFieldBytes, + kMaxFailedDetail] dışında ya da neden bilinmiyorsa nullopt.
std::optional<Failure> decode_failed(std::string_view payload);

// Rendered yükünün beklenen boyu: 8 + w·h·4; taşarsa nullopt.
std::optional<std::uint64_t> rendered_length(std::uint32_t width, std::uint32_t height);

}  // namespace noteapp::pdfview::protocol
