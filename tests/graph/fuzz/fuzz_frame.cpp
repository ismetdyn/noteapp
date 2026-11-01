// libFuzzer: uzak günlük çerçevesi ve değer çözücüsü (güvenilmez girdi). Kabul edilen çerçeve yeniden
// kodlanınca aynı baytları vermeli: çözücü bir şeyi sessizce düşürürse iki cihaz farklı içerik görürdü.
#include "graph/op.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> input = std::as_bytes(std::span(data, size));
    const Result<Frame> frame = decode_frame(input);
    if (frame.has_value()) {
        const std::string encoded = encode_frame(*frame);
        const std::string original(reinterpret_cast<const char*>(data), size);
        if (encoded != original) {
            std::fputs("decode_frame accepted a non-canonical frame\n", stderr);
            std::abort();
        }
    }
    (void)decode_value(input);
    return 0;
}
