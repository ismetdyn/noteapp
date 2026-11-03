#include "graph/id.hpp"
#include "graph/oplog.hpp"
#include "sync_test_support.hpp"

#include <algorithm>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;

namespace {

const DeviceId kDevice = [] {
    DeviceId device{};
    for (std::size_t i = 0; i < device.size(); ++i)
        device[i] = static_cast<std::byte>(0xA0 + i);
    return device;
}();

const std::vector<std::string> kPayloads = {"first", std::string("se\0cond\r\n", 9), "third payload"};

std::string sample_log() {
    std::string log = encode_log_header(kDevice);
    for (const std::string& payload : kPayloads)
        log += encode_log_frame(payload);
    return log;
}

// Dosya sonuna dek okur: gerçek okuyucu, gerçek dosya. payloads kopyadır.
struct LogScan {
    DeviceId device{};
    std::vector<std::string> payloads;
    std::size_t valid_end = 0;
    bool partial_tail = false;
    std::optional<Error> error;
};

LogScan scan(const std::string& file) {
    static const noteapp::test::TempDir directory;
    const std::filesystem::path path = directory.path() / "scan.oplog";
    noteapp::graph::test::write_file(path, file);
    Result<std::optional<LogReader>> opened = LogReader::open(path, kDefaultFrameBytes);
    REQUIRE(opened.has_value());
    REQUIRE(opened->has_value());
    LogReader& reader = **opened;
    LogScan result;
    const LogReader::Status header = reader.read_header();
    if (header == LogReader::Status::PartialTail) {
        result.partial_tail = true;
        return result;
    }
    if (header == LogReader::Status::Damaged) {
        result.error = reader.error();
        return result;
    }
    result.device = reader.device();
    result.valid_end = reader.offset();
    while (true) {
        const LogReader::Status status = reader.next();
        if (status == LogReader::Status::End)
            return result;
        if (status == LogReader::Status::PartialTail) {
            result.partial_tail = true;
            return result;
        }
        if (status == LogReader::Status::Damaged) {
            result.error = reader.error();
            return result;
        }
        const std::span<const std::byte> payload = reader.payload();
        result.payloads.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
        result.valid_end = reader.offset();
    }
}

void put_u32_le(std::string& out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof value; ++i)
        out[offset + i] = static_cast<char>(value >> (i * 8U));
}

}  // namespace

TEST_CASE("log_scan_returns_every_payload_byte_exact") {
    const std::string log = sample_log();
    const LogScan result = scan(log);
    REQUIRE_FALSE(result.error.has_value());
    CHECK_FALSE(result.partial_tail);
    CHECK(result.device == kDevice);
    CHECK(result.valid_end == log.size());
    REQUIRE(result.payloads.size() == kPayloads.size());
    for (std::size_t i = 0; i < kPayloads.size(); ++i)
        CHECK(result.payloads[i] == kPayloads[i]);
}

TEST_CASE("log_header_is_magic_version_device") {
    const std::string header = encode_log_header(kDevice);
    REQUIRE(header.size() == kLogHeaderBytes);
    CHECK(header.substr(0, 4) == "NGOL");
    // Varsayılan v1: bölünmesi gereken çerçeve yoksa eski sürümler de okur.
    CHECK(static_cast<std::uint8_t>(header[4]) == kLogVersionWhole);
    CHECK(static_cast<std::uint8_t>(encode_log_header(kDevice, kLogVersionParts)[4]) == kLogVersion);
    CHECK(std::memcmp(header.data() + 5, kDevice.data(), kDevice.size()) == 0);
}

TEST_CASE("log_frame_is_length_digest_payload") {
    const std::string frame = encode_log_frame("abc");
    REQUIRE(frame.size() == kFrameEnvelopeBytes + 3);
    CHECK(frame.substr(0, 4) == std::string("\x03\0\0\0", 4));
    const auto digest = sha256(std::as_bytes(std::span("abc", 3)));
    CHECK(std::memcmp(frame.data() + 4, digest.data(), kFrameDigestBytes) == 0);
    CHECK(frame.substr(kFrameEnvelopeBytes) == "abc");
}

TEST_CASE("log_file_name_is_hex_device_with_extension") {
    CHECK(log_file_name(kDevice) == "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf.oplog");
}

TEST_CASE("truncation_at_every_byte_gives_a_valid_prefix_and_no_error") {
    const std::string log = sample_log();
    // Çerçeve sınırında kesilen dosya eksik değildir, yalnız kısadır.
    std::vector<std::size_t> boundaries = {kLogHeaderBytes};
    for (const std::string& payload : kPayloads)
        boundaries.push_back(boundaries.back() + kFrameEnvelopeBytes + payload.size());
    std::size_t last_count = 0;
    for (std::size_t size = 0; size < log.size(); ++size) {
        INFO("size=" << size);
        const std::string cut = log.substr(0, size);  // payloads bu dizgenin üzerinde görünümdür
        const LogScan result = scan(cut);
        CHECK_FALSE(result.error.has_value());
        CHECK(result.partial_tail != std::ranges::contains(boundaries, size));
        CHECK(result.valid_end <= size);
        REQUIRE(result.payloads.size() <= kPayloads.size());
        CHECK(result.payloads.size() >= last_count);
        last_count = result.payloads.size();
        for (std::size_t i = 0; i < result.payloads.size(); ++i)
            CHECK(result.payloads[i] == kPayloads[i]);
    }
    CHECK(last_count == kPayloads.size() - 1);
}

// Her konum ve her tek-bit çevirme: bozuk içerik asla geçerli payload olarak çıkmaz. Başlığın özeti yoktur:
// dev baytındaki çevirme başka bir cihaz adı üretir, onu dosya adı karşılaştırması yakalar.
TEST_CASE("byte_flip_sweep_never_yields_altered_payload") {
    const std::string log = sample_log();
    REQUIRE(log.size() > kLogHeaderBytes);
    constexpr std::size_t kDeviceOffset = kLogHeaderBytes - std::tuple_size_v<DeviceId>;
    for (std::size_t at = 0; at < log.size(); ++at) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            std::string damaged = log;
            damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ (1U << bit));
            INFO("at=" << at << " bit=" << bit);
            const LogScan result = scan(damaged);
            if (at >= kDeviceOffset && at < kLogHeaderBytes) {
                CHECK(result.device != kDevice);
                CHECK(log_file_name(result.device) != log_file_name(kDevice));
                continue;
            }
            CHECK((result.error.has_value() || result.partial_tail));
            REQUIRE(result.payloads.size() < kPayloads.size());
            for (std::size_t i = 0; i < result.payloads.size(); ++i)
                CHECK(result.payloads[i] == kPayloads[i]);
        }
    }
}

TEST_CASE("corrupt_frame_stops_the_log_and_keeps_the_prefix") {
    std::string log = sample_log();
    const std::size_t second_frame = kLogHeaderBytes + kFrameEnvelopeBytes + kPayloads[0].size();
    REQUIRE(log.size() > second_frame + kFrameEnvelopeBytes);
    log[second_frame + kFrameEnvelopeBytes] ^= 0x01;
    const LogScan result = scan(log);
    REQUIRE(result.error.has_value());
    CHECK(result.error->code == Error::Code::BadFrame);
    CHECK_FALSE(result.partial_tail);
    CHECK(result.valid_end == second_frame);
    REQUIRE(result.payloads.size() == 1);
    CHECK(result.payloads[0] == kPayloads[0]);
}

TEST_CASE("oversized_length_is_rejected_before_it_is_read") {
    std::string log = encode_log_header(kDevice) + encode_log_frame("x");
    REQUIRE(log.size() >= kLogHeaderBytes + sizeof(std::uint32_t));
    const auto over = static_cast<std::uint32_t>(kDefaultFrameBytes + 1);
    put_u32_le(log, kLogHeaderBytes, over);
    const LogScan result = scan(log);
    REQUIRE(result.error.has_value());
    CHECK(result.error->code == Error::Code::BadFrame);
    CHECK_FALSE(result.partial_tail);
    CHECK(result.payloads.empty());

    // Sınırdaki uzunluk bozuk sayılmaz: dosya henüz o kadar gelmemiştir.
    put_u32_le(log, kLogHeaderBytes, static_cast<std::uint32_t>(kDefaultFrameBytes));
    const LogScan at_limit = scan(log);
    CHECK_FALSE(at_limit.error.has_value());
    CHECK(at_limit.partial_tail);
}

TEST_CASE("header_rejects_foreign_magic_and_newer_version") {
    std::string foreign = sample_log();
    REQUIRE(foreign.size() > 4);
    foreign[0] = 'X';
    REQUIRE(scan(foreign).error.has_value());
    CHECK(scan(foreign).error->code == Error::Code::BadFrame);
    CHECK(scan(foreign).payloads.empty());

    std::string newer = sample_log();
    newer[4] = static_cast<char>(kLogVersion + 1);
    REQUIRE(scan(newer).error.has_value());
    CHECK(scan(newer).error->code == Error::Code::TooNew);
    CHECK(scan(newer).payloads.empty());
}

TEST_CASE("foreign_magic_in_a_short_file_is_an_error_not_a_partial_tail") {
    const LogScan result = scan("XG");
    REQUIRE(result.error.has_value());
    CHECK_FALSE(result.partial_tail);
}

TEST_CASE("empty_file_is_a_partial_header") {
    const LogScan result = scan("");
    CHECK_FALSE(result.error.has_value());
    CHECK(result.partial_tail);
    CHECK(result.payloads.empty());
    CHECK(result.valid_end == 0);
}
