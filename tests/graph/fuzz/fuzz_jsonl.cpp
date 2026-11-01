// `note graph import` satır ayrıştırıcısı: düşmanca JSON düşürmez, asmaz, sınırsız ayırmaz.
#include "cli/json_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    noteapp::cli::JsonParser parser(text);
    static_cast<void>(parser.parse());
    return 0;
}
