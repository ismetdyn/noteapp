// libFuzzer: uzak günlük dosyasının okuyucusu (LogReader; v1 tam çerçeve, v2 parçalı grup). Girdi dosyanın
// tamamıdır. Değişmezler: süreç düşmez, birleştirilen çerçeve grup sınırını aşmaz, okuma dosyanın içinde
// kalır, kabul edilen çerçeve yeniden kodlanınca aynı baytları verir.
#include "graph/op.hpp"
#include "graph/oplog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;

namespace {

// Küçük sınırlar: büyük girdi üretmeden kayıt ve grup sınırlarına varılır.
constexpr std::size_t kFrameBytes = 4096;
constexpr std::size_t kGroupBytes = 64U * 1024U;
constexpr FrameLimits kRecordLimits{kFrameBytes, 64, 64};

void require(bool condition, const char* what) {
    if (!condition) {
        std::fputs(what, stderr);
        std::abort();
    }
}

const std::filesystem::path& scratch_file() {
    static const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("fuzz_log." + std::to_string(::getpid()) + ".oplog");
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    {
        std::ofstream out(scratch_file(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    Result<std::optional<LogReader>> opened = LogReader::open(scratch_file(), kFrameBytes, kGroupBytes);
    if (!opened || !opened->has_value())
        return 0;
    LogReader& reader = **opened;
    if (reader.read_header() != LogReader::Status::Frame)
        return 0;
    while (reader.next() == LogReader::Status::Frame) {
        require(reader.payload().size() <= kGroupBytes, "group larger than its limit\n");
        require(reader.parts() >= 1, "frame without parts\n");
        require(reader.offset() <= reader.file_size(), "offset past the end of the file\n");
        const Result<Frame> frame =
            decode_frame(reader.payload(), logical_frame_limits(kRecordLimits, reader.parts()));
        if (frame.has_value()) {
            const std::string encoded = encode_frame(*frame);
            require(
                std::equal(encoded.begin(), encoded.end(),
                           reinterpret_cast<const char*>(reader.payload().data()),
                           reinterpret_cast<const char*>(reader.payload().data()) + reader.payload().size()),
                "decode_frame accepted a non-canonical frame\n");
        }
    }
    return 0;
}
