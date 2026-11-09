#include "pdfview/protocol.hpp"

#include <algorithm>
#include <bit>
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
    case Kind::Opened:
    case Kind::TextResult:
    case Kind::Rendered:
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
