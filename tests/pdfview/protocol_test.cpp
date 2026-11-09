// PDF1b: işçi tel protokolü (pdfview/protocol.hpp): gidiş-dönüş ve bozuk çerçevenin reddi.
#include "pdfview/protocol.hpp"

#include <doctest/doctest.h>
#include <limits>
#include <string>

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
    open[0] = '\x02';  // başka sürüm
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
