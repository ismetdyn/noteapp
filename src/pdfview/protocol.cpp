#include "pdfview/protocol.hpp"

#include "pdfview/page_text.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace noteapp::pdfview::protocol {

namespace {

constexpr int kBitsPerByte = 8;
constexpr std::uint64_t kByteMask = 0xFFU;
constexpr std::uint64_t kBytesPerPixel = 4;

bool known_kind(std::uint32_t value) {
    switch (static_cast<Kind>(value)) {
    case Kind::Open:
    case Kind::Text:
    case Kind::Render:
    case Kind::Structured:
    case Kind::Opened:
    case Kind::TextResult:
    case Kind::Rendered:
    case Kind::StructuredResult:
    case Kind::Failed:
        return true;
    }
    return false;
}

bool known_reason(std::uint32_t value) {
    return value >= static_cast<std::uint32_t>(FailReason::Invalid) &&
           value <= static_cast<std::uint32_t>(FailReason::NoMemory);
}

std::string frame(Kind kind, std::string_view payload) {
    std::string out = encode_header(kind, payload.size());
    out.append(payload);
    return out;
}

constexpr std::uint8_t kLineJoined = 1;
constexpr float kMinPageSize = 1.0F;  // punto

void put_f32(std::string& out, float value) {
    put_u32(out, std::bit_cast<std::uint32_t>(value));
}

// Güvenilmez yükün sıralı okuyucusu: her okuma kalan baytı denetler, yetmezse false.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& out) {
        if (left() < 1)
            return false;
        out = static_cast<std::uint8_t>(bytes_[at_++]);
        return true;
    }
    bool u32(std::uint32_t& out) {
        if (left() < 4)
            return false;
        out = get_u32(bytes_, at_);
        at_ += 4;
        return true;
    }
    bool f32(float& out) {
        std::uint32_t raw = 0;
        if (!u32(raw))
            return false;
        out = std::bit_cast<float>(raw);
        return std::isfinite(out);
    }
    bool bytes(std::size_t size, std::string_view& out) {
        if (left() < size)
            return false;
        out = bytes_.substr(at_, size);
        at_ += size;
        return true;
    }
    std::size_t left() const { return bytes_.size() - at_; }

private:
    std::string_view bytes_;
    std::size_t at_ = 0;
};

// [-pay, limit + pay] içinde ve first <= second.
bool within(float first, float second, float limit) {
    return first >= -kPageTolerance && first <= second && second <= limit + kPageTolerance;
}

// Geçerli UTF-8 ise kod noktası sayısı.
std::optional<std::size_t> code_points(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size(); ++count) {
        std::size_t length = 1;
        if (!detail::decode_one(text.substr(at), length))
            return std::nullopt;
        at += length;
    }
    return count;
}

bool read_span(Reader& in, float width, std::uint64_t& text_left, TextSpan& span) {
    std::uint32_t bytes = 0;
    std::string_view text;
    if (!in.u8(span.style) || (span.style & ~kStyleMask) != 0 || !in.f32(span.size) || span.size < 0.0F ||
        !in.u32(bytes) || bytes == 0 || bytes > text_left || !in.bytes(bytes, text))
        return false;
    text_left -= bytes;
    const std::optional<std::size_t> chars = code_points(text);
    // Kenar sayısı metinden gelir; kalan bayt yetmezse hiç ayrılmaz.
    if (!chars || in.left() / 8 < *chars)
        return false;
    span.text = std::string(text);
    span.edges.reserve(*chars * 2);
    for (std::size_t i = 0; i < *chars; ++i) {
        float left = 0.0F;
        float right = 0.0F;
        if (!in.f32(left) || !in.f32(right) || !within(left, right, width))
            return false;
        span.edges.push_back(left);
        span.edges.push_back(right);
    }
    return true;
}

bool read_line(Reader& in, const PageText& page, std::uint64_t& text_left, TextLine& line) {
    std::uint8_t flags = 0;
    std::uint32_t spans = 0;
    if (!in.u8(flags) || (flags & ~kLineJoined) != 0 || !in.f32(line.top) || !in.f32(line.bottom) ||
        !within(line.top, line.bottom, page.height) || !in.u32(spans) || spans == 0)
        return false;
    line.joined = (flags & kLineJoined) != 0;
    for (std::uint32_t i = 0; i < spans; ++i) {
        TextSpan span;
        if (!read_span(in, page.width, text_left, span))
            return false;
        line.spans.push_back(std::move(span));
    }
    return true;
}

}  // namespace

void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out += static_cast<char>((value >> (i * kBitsPerByte)) & kByteMask);
}

void put_u64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out += static_cast<char>((value >> (i * kBitsPerByte)) & kByteMask);
}

std::uint32_t get_u32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = (value << kBitsPerByte) | static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]);
    return value;
}

std::uint64_t get_u64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << kBitsPerByte) | static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]);
    return value;
}

std::string encode_header(Kind kind, std::uint64_t length) {
    std::string out;
    out.reserve(kHeaderBytes);
    put_u32(out, static_cast<std::uint32_t>(kind));
    put_u32(out, 0);
    put_u64(out, length);
    return out;
}

std::optional<Header> decode_header(std::string_view bytes) {
    if (bytes.size() != kHeaderBytes)
        return std::nullopt;
    const std::uint32_t kind = get_u32(bytes, 0);
    if (!known_kind(kind) || get_u32(bytes, 4) != 0)
        return std::nullopt;
    return Header{static_cast<Kind>(kind), get_u64(bytes, 8)};
}

std::string encode_open_head(const OpenFields& fields, std::uint64_t pdf_size) {
    std::string out = encode_header(Kind::Open, kOpenFieldBytes + pdf_size);
    put_u32(out, fields.version);
    put_u64(out, fields.parent_pid);
    put_u64(out, fields.pages);
    put_u64(out, fields.render_pixels);
    put_u64(out, fields.text_bytes);
    put_u64(out, fields.context_memory);
    return out;
}

std::optional<OpenFields> decode_open(std::string_view payload) {
    if (payload.size() < kOpenFieldBytes || get_u32(payload, 0) != kVersion)
        return std::nullopt;
    return OpenFields{.version = kVersion,
                      .parent_pid = get_u64(payload, 4),
                      .pages = get_u64(payload, 12),
                      .render_pixels = get_u64(payload, 20),
                      .text_bytes = get_u64(payload, 28),
                      .context_memory = get_u64(payload, 36)};
}

std::string encode_text_request(std::uint32_t page) {
    std::string payload;
    put_u32(payload, page);
    return frame(Kind::Text, payload);
}

std::string encode_structured_request(std::uint32_t page) {
    std::string payload;
    put_u32(payload, page);
    return frame(Kind::Structured, payload);
}

std::string encode_render_request(std::uint32_t page, float scale) {
    std::string payload;
    put_u32(payload, page);
    put_u32(payload, std::bit_cast<std::uint32_t>(scale));
    return frame(Kind::Render, payload);
}

std::optional<std::uint32_t> decode_text_request(std::string_view payload) {
    if (payload.size() != 4)
        return std::nullopt;
    return get_u32(payload, 0);
}

std::optional<RenderRequest> decode_render_request(std::string_view payload) {
    if (payload.size() != 8)
        return std::nullopt;
    return RenderRequest{get_u32(payload, 0), std::bit_cast<float>(get_u32(payload, 4))};
}

std::string encode_opened(std::uint32_t page_count) {
    std::string payload;
    put_u32(payload, page_count);
    return frame(Kind::Opened, payload);
}

std::string encode_text_result(std::string_view text) {
    return frame(Kind::TextResult, text);
}

std::string encode_rendered(std::uint32_t width, std::uint32_t height, std::string_view rgba) {
    std::string payload;
    payload.reserve(kRenderedFieldBytes + rgba.size());
    put_u32(payload, width);
    put_u32(payload, height);
    payload.append(rgba);
    return frame(Kind::Rendered, payload);
}

std::string encode_structured_result(const PageText& page) {
    std::string payload;
    put_f32(payload, page.width);
    put_f32(payload, page.height);
    put_u32(payload, static_cast<std::uint32_t>(page.blocks.size()));
    for (const TextBlock& block : page.blocks) {
        put_u32(payload, static_cast<std::uint32_t>(block.lines.size()));
        for (const TextLine& line : block.lines) {
            payload += static_cast<char>(line.joined ? kLineJoined : 0);
            put_f32(payload, line.top);
            put_f32(payload, line.bottom);
            put_u32(payload, static_cast<std::uint32_t>(line.spans.size()));
            for (const TextSpan& span : line.spans) {
                payload += static_cast<char>(span.style);
                put_f32(payload, span.size);
                put_u32(payload, static_cast<std::uint32_t>(span.text.size()));
                payload += span.text;
                for (const float edge : span.edges)
                    put_f32(payload, edge);
            }
        }
    }
    return frame(Kind::StructuredResult, payload);
}

std::uint64_t structured_length_cap(std::uint64_t text_cap) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    if (text_cap > (kMax - kStructuredHeadBytes) / kStructuredBytesPerTextByte)
        return kMax;
    return kStructuredHeadBytes + (text_cap * kStructuredBytesPerTextByte);
}

std::optional<PageText> decode_structured(std::string_view payload, std::uint64_t text_cap) {
    Reader in(payload);
    PageText page;
    std::uint32_t blocks = 0;
    // Dürüst işçi sayfayı en az 1 pt yapar (engine): daha küçüğü (denormal dahil) ölçek bölmesini taşırır.
    if (!in.f32(page.width) || !in.f32(page.height) || !(page.width >= kMinPageSize) ||
        !(page.height >= kMinPageSize) || !in.u32(blocks))
        return std::nullopt;
    std::uint64_t text_left = text_cap;
    // Sayılar yalnız döngü sınırıdır: her öğe bayt tükettiğinden yalan sayı kısa yükte biter.
    for (std::uint32_t b = 0; b < blocks; ++b) {
        std::uint32_t lines = 0;
        if (!in.u32(lines) || lines == 0)
            return std::nullopt;
        TextBlock block;
        for (std::uint32_t l = 0; l < lines; ++l) {
            TextLine line;
            if (!read_line(in, page, text_left, line))
                return std::nullopt;
            block.lines.push_back(std::move(line));
        }
        page.blocks.push_back(std::move(block));
    }
    if (in.left() != 0)
        return std::nullopt;
    return page;
}

std::string encode_failed(const Failure& failure) {
    std::string payload;
    put_u32(payload, static_cast<std::uint32_t>(failure.reason));
    put_u64(payload, failure.a);
    put_u64(payload, failure.b);
    payload.append(std::string_view(failure.detail).substr(0, kMaxFailedDetail));
    return frame(Kind::Failed, payload);
}

std::optional<Failure> decode_failed(std::string_view payload) {
    if (payload.size() < kFailedFieldBytes || payload.size() > kFailedFieldBytes + kMaxFailedDetail)
        return std::nullopt;
    const std::uint32_t reason = get_u32(payload, 0);
    if (!known_reason(reason))
        return std::nullopt;
    return Failure{static_cast<FailReason>(reason), get_u64(payload, 4), get_u64(payload, 12),
                   std::string(payload.substr(kFailedFieldBytes))};
}

std::optional<std::uint64_t> rendered_length(std::uint32_t width, std::uint32_t height) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;  // < 2^64
    if (pixels > (std::numeric_limits<std::uint64_t>::max() - kRenderedFieldBytes) / kBytesPerPixel)
        return std::nullopt;
    return kRenderedFieldBytes + pixels * kBytesPerPixel;
}

}  // namespace noteapp::pdfview::protocol
