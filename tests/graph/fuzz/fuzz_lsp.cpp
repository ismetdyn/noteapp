// LSP sunucusunun stdout'u güvenilmez: çerçeveleyici ve ayrıştırıcılar düşmanca baytta düşmez, asmaz,
// sınırsız ayırmaz. İlk bayt parça boyunu seçer: mesajın okumalar arasında bölündüğü yollar da denenir.
#include "core/lsp_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace noteapp::core::lsp;
    if (size == 0)
        return 0;
    const std::size_t chunk = std::size_t{data[0]} + 1;
    const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
    FrameReader reader;
    for (std::size_t at = 0; at < bytes.size(); at += chunk) {
        reader.append(bytes.substr(at, chunk));
        while (true) {
            auto next = reader.next();
            if (!next || !*next)
                break;
            const auto message = parse_message(**next);
            if (!message)
                continue;
            static_cast<void>(reply_to_server_request(*message));
            static_cast<void>(negotiated_encoding(*message));
            static_cast<void>(parse_publish_diagnostics(*message));
            static_cast<void>(parse_completion_result(*message));
        }
    }
    constexpr std::size_t kColumnProbeBytes = 64;
    const std::string_view line = bytes.substr(0, std::min(bytes.size(), kColumnProbeBytes));
    static_cast<void>(utf16_to_byte_column(line, data[0]));
    static_cast<void>(byte_to_utf16_column(line, data[0]));
    return 0;
}
