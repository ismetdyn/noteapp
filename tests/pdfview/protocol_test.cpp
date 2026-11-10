// PDF1b: işçi tel protokolü (pdfview/protocol.hpp): gidiş-dönüş ve bozuk çerçevenin reddi.
#include "pdfview/protocol.hpp"

#include <array>
#include <bit>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <utility>

namespace proto = noteapp::pdfview::protocol;

namespace {

std::string payload_of(const std::string& frame) {
    return frame.substr(proto::kHeaderBytes);
}

}  // namespace

TEST_CASE("frames round-trip with little-endian fixed-width fields") {
    const std::string header = proto::encode_header(proto::Kind::Rendered, 0x0102030405060708ULL);
    REQUIRE(header.size() == proto::kHeaderBytes);
    CHECK(header[0] == '\x83');
    CHECK(header[8] == '\x08');  // küçük-sonlu
    const auto decoded = proto::decode_header(header);
    REQUIRE(decoded);
    CHECK(decoded->kind == proto::Kind::Rendered);
    CHECK(decoded->length == 0x0102030405060708ULL);

    const proto::OpenFields fields{.version = proto::kVersion,
                                   .parent_pid = 4242,
                                   .pages = 1,
                                   .render_pixels = 2,
                                   .text_bytes = 3,
                                   .context_memory = std::numeric_limits<std::uint64_t>::max()};
    const std::string open = proto::encode_open_head(fields, 6) + "%PDF-x";
    const auto open_fields = proto::decode_open(payload_of(open));
    REQUIRE(open_fields);
    CHECK(open_fields->parent_pid == 4242);
    CHECK(open_fields->text_bytes == 3);
    CHECK(open_fields->context_memory == std::numeric_limits<std::uint64_t>::max());
    CHECK(payload_of(open).substr(proto::kOpenFieldBytes) == "%PDF-x");

    CHECK(proto::decode_text_request(payload_of(proto::encode_text_request(7))) == 7U);
    const auto render = proto::decode_render_request(payload_of(proto::encode_render_request(3, 1.5F)));
    REQUIRE(render);
    CHECK(render->page == 3);
    CHECK(render->scale == 1.5F);

    const auto failure = proto::decode_failed(payload_of(
        proto::encode_failed({.reason = proto::FailReason::RenderTooLarge, .a = 10, .b = 20, .detail = "d"})));
    REQUIRE(failure);
    CHECK(failure->reason == proto::FailReason::RenderTooLarge);
    CHECK(failure->a == 10);
    CHECK(failure->b == 20);
    CHECK(failure->detail == "d");
}

TEST_CASE("a truncated, unknown or non-zero-reserved header is rejected") {
    const std::string header = proto::encode_header(proto::Kind::Opened, 4);
    CHECK_FALSE(proto::decode_header(header.substr(0, proto::kHeaderBytes - 1)));
    std::string unknown = header;
    unknown[0] = '\x42';
    CHECK_FALSE(proto::decode_header(unknown));
    std::string reserved = header;
    reserved[5] = '\x01';
    CHECK_FALSE(proto::decode_header(reserved));
}

TEST_CASE("request payloads with the wrong size or version are rejected") {
    CHECK_FALSE(proto::decode_text_request("abc"));
    CHECK_FALSE(proto::decode_text_request("abcde"));
    CHECK_FALSE(proto::decode_render_request("1234567"));
    std::string open = payload_of(proto::encode_open_head({}, 0));
    CHECK_FALSE(proto::decode_open(open.substr(0, proto::kOpenFieldBytes - 1)));
    open[0] = static_cast<char>(proto::kVersion + 1);  // başka sürüm
    CHECK_FALSE(proto::decode_open(open));
}

TEST_CASE("a Failed payload must carry a known reason and at most 256 bytes of detail") {
    const proto::Failure invalid{.reason = proto::FailReason::Invalid, .a = 0, .b = 0, .detail = {}};
    std::string payload = payload_of(proto::encode_failed(invalid));
    CHECK(proto::decode_failed(payload));
    CHECK_FALSE(proto::decode_failed(payload.substr(0, proto::kFailedFieldBytes - 1)));
    std::string unknown = payload;
    unknown[0] = '\x63';
    CHECK_FALSE(proto::decode_failed(unknown));
    CHECK_FALSE(proto::decode_failed(payload + std::string(proto::kMaxFailedDetail + 1, 'x')));
    // Kodlayıcı ayrıntıyı keser: üretilen çerçeve her zaman geçerli.
    const std::string long_detail(1000, 'y');
    CHECK(proto::decode_failed(payload_of(proto::encode_failed(
        {.reason = proto::FailReason::Invalid, .a = 0, .b = 0, .detail = long_detail}))));
}

TEST_CASE("the expected RGBA length is 8 + w*h*4 and overflow is detected") {
    CHECK(proto::rendered_length(2, 3) == 8U + 2U * 3U * 4U);
    CHECK(proto::rendered_length(0, 5) == 8U);
    constexpr std::uint32_t kMax = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(proto::rendered_length(kMax, kMax));
    CHECK(proto::rendered_length(0x7FFFFFFFU, 0x7FFFFFFFU).has_value());
}

namespace {

namespace pv = noteapp::pdfview;

constexpr std::uint64_t kNoCap = std::numeric_limits<std::uint64_t>::max();

pv::TextSpan span_at(std::string text, std::uint8_t style, float size, float x) {
    pv::TextSpan out{.text = std::move(text), .style = style, .size = size, .edges = {}};
    for (const char byte : out.text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) == 0x80U)
            continue;  // kod noktası başına iki kenar
        out.edges.push_back(x);
        out.edges.push_back(x + 5.0F);
        x += 5.0F;
    }
    return out;
}

// 200x100 pt sayfa: iki blok; ilkinde iki satır (ikincisi iki parça), her kod noktası 5 pt.
pv::PageText sample_page() {
    pv::TextLine first{.top = 10.0F, .bottom = 22.0F, .joined = true,
                       .spans = {span_at("Başlık-", pv::kStyleBold, 18.0F, 0.0F)}};
    pv::TextLine second{.top = 24.0F, .bottom = 34.0F, .joined = false,
                        .spans = {span_at("düz ", 0, 10.0F, 0.0F),
                                  span_at("mono", pv::kStyleMono | pv::kStyleItalic, 10.0F, 20.0F)}};
    pv::TextLine third{.top = 50.0F, .bottom = 60.0F, .joined = false, .spans = {span_at("son", 0, 10.0F, 185.0F)}};
    return {.width = 200.0F, .height = 100.0F, .blocks = {{.lines = {first, second}}, {.lines = {third}}}};
}

std::string structured_payload(const pv::PageText& page) {
    return payload_of(proto::encode_structured_result(page));
}

// Yükün `at` baytındaki u32'yi (küçük-sonlu) değiştirir. replace ile: indeksle bayt yazımı mingw GCC -O3'te
// -Wstringop-overflow yanlış alarmı veriyordu (yerel std::string nesnesinin boyutunu hedef sanıyor).
std::string with_u32(std::string payload, std::size_t at, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>((value >> (8 * i)) & 0xFFU);
    payload.replace(at, bytes.size(), bytes.data(), bytes.size());
    return payload;
}

// Yükün `at` baytındaki f32'yi değiştirir.
std::string with_f32(std::string payload, std::size_t at, float value) {
    return with_u32(std::move(payload), at, std::bit_cast<std::uint32_t>(value));
}

}  // namespace

TEST_CASE("a structured page round-trips exactly through the wire") {
    const pv::PageText page = sample_page();
    const std::string frame = proto::encode_structured_result(page);
    const auto header = proto::decode_header(frame.substr(0, proto::kHeaderBytes));
    REQUIRE(header);
    CHECK(header->kind == proto::Kind::StructuredResult);
    CHECK(proto::decode_text_request(payload_of(proto::encode_structured_request(9))) == 9U);
    const auto decoded = proto::decode_structured(payload_of(frame), kNoCap);
    REQUIRE(decoded);
    CHECK(*decoded == page);
    // Toplam metin tam sınırda geçer, bir bayt azında bozuk çerçevedir.
    std::uint64_t text = 0;
    for (const auto& block : page.blocks)
        for (const auto& line : block.lines)
            for (const auto& span : line.spans)
                text += span.text.size();
    CHECK(proto::decode_structured(payload_of(frame), text));
    CHECK_FALSE(proto::decode_structured(payload_of(frame), text - 1));
    // Çerçeve sınırı metin sınırından türer ve taşmaz.
    CHECK(payload_of(frame).size() <= proto::structured_length_cap(text));
    CHECK(proto::structured_length_cap(kNoCap) == kNoCap);
    CHECK(proto::structured_length_cap(0) == proto::kStructuredHeadBytes);
}

TEST_CASE("a hostile structured payload is rejected, never trusted") {
    const std::string good = structured_payload(sample_page());
    REQUIRE(proto::decode_structured(good, kNoCap));
    // Alan yerleri (protocol.hpp): baş 12 bayt; blok 0 {u32 satır @12}; satır {u8 bayrak @16, f32 top @17,
    // f32 bottom @21, u32 parça @25}; parça {u8 style @29, f32 size @30, u32 bayt @34, metin @38, kenarlar}.
    constexpr std::size_t kLineFlags = 16;
    constexpr std::size_t kLineTop = 17;
    constexpr std::size_t kStyle = 29;
    constexpr std::size_t kTextLength = 34;
    constexpr std::size_t kText = 38;
    const std::size_t first_edge = kText + std::string("Başlık-").size();
    SUBCASE("lying counts end at the short payload without allocating them") {
        CHECK_FALSE(proto::decode_structured(with_u32(good.substr(0, proto::kStructuredHeadBytes), 8, 0xFFFFFFFFU),
                                             kNoCap));
        CHECK_FALSE(proto::decode_structured(with_u32(good, 8, 0xFFFFFFFFU), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_u32(good, 12, 0xFFFFFFFFU), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_u32(good, 25, 0xFFFFFFFFU), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_u32(good, kTextLength, 0xFFFFFFFFU), kNoCap));
    }
    SUBCASE("non-finite coordinates and sizes") {
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            CAPTURE(bad);
            CHECK_FALSE(proto::decode_structured(with_f32(good, 0, bad), kNoCap));  // sayfa genişliği
            CHECK_FALSE(proto::decode_structured(with_f32(good, kLineTop, bad), kNoCap));
            CHECK_FALSE(proto::decode_structured(with_f32(good, kStyle + 1, bad), kNoCap));  // boy
            CHECK_FALSE(proto::decode_structured(with_f32(good, first_edge + 4, bad), kNoCap));
        }
    }
    SUBCASE("coordinates outside the page or inverted") {
        CHECK(proto::decode_structured(with_f32(good, first_edge + 4, 200.0F + proto::kPageTolerance), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_f32(good, first_edge + 4, 500.0F), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_f32(good, first_edge, -5.0F), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_f32(good, first_edge, 6.0F), kNoCap));  // x0 > x1
        CHECK_FALSE(proto::decode_structured(with_f32(good, kLineTop, 30.0F), kNoCap));  // top > bottom
        CHECK_FALSE(proto::decode_structured(with_f32(good, kLineTop + 4, 150.0F), kNoCap));
        CHECK_FALSE(proto::decode_structured(with_f32(good, 4, 0.0F), kNoCap));  // sayfa yüksekliği 0
        CHECK_FALSE(proto::decode_structured(with_f32(good, kStyle + 1, -1.0F), kNoCap));  // negatif boy
    }
    SUBCASE("invalid UTF-8, unknown bits, empty elements and trailing bytes") {
        std::string utf8 = good;
        utf8[kText + 1] = '\xFF';
        CHECK_FALSE(proto::decode_structured(utf8, kNoCap));
        std::string style = good;
        style[kStyle] = '\x08';
        CHECK_FALSE(proto::decode_structured(style, kNoCap));
        std::string flags = good;
        flags[kLineFlags] = '\x02';
        CHECK_FALSE(proto::decode_structured(flags, kNoCap));
        CHECK_FALSE(proto::decode_structured(good + "x", kNoCap));
        CHECK_FALSE(proto::decode_structured(good.substr(0, good.size() - 1), kNoCap));
        pv::PageText empty_line = sample_page();
        empty_line.blocks[1].lines[0].spans.clear();
        CHECK_FALSE(proto::decode_structured(structured_payload(empty_line), kNoCap));
        pv::PageText empty_block = sample_page();
        empty_block.blocks[1].lines.clear();
        CHECK_FALSE(proto::decode_structured(structured_payload(empty_block), kNoCap));
        pv::PageText no_text = sample_page();
        no_text.blocks[1].lines[0].spans[0].text.clear();
        no_text.blocks[1].lines[0].spans[0].edges.clear();
        CHECK_FALSE(proto::decode_structured(structured_payload(no_text), kNoCap));
    }
    SUBCASE("a page narrower or shorter than 1 pt is rejected") {
        for (const float bad : {0.0F, -1.0F, std::numeric_limits<float>::denorm_min(), 0.5F}) {
            CAPTURE(bad);
            const pv::PageText blank{.width = bad, .height = 10.0F, .blocks = {}};
            CHECK_FALSE(proto::decode_structured(structured_payload(blank), kNoCap));
            const pv::PageText flat{.width = 10.0F, .height = bad, .blocks = {}};
            CHECK_FALSE(proto::decode_structured(structured_payload(flat), kNoCap));
        }
        const pv::PageText smallest{.width = 1.0F, .height = 1.0F, .blocks = {}};
        CHECK(proto::decode_structured(structured_payload(smallest), kNoCap) == smallest);
    }
    SUBCASE("a page without text is valid") {
        const pv::PageText blank{.width = 10.0F, .height = 10.0F, .blocks = {}};
        CHECK(proto::decode_structured(structured_payload(blank), 0) == blank);
    }
}
