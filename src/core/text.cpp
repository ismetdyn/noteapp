#include "core/text.hpp"

#include <array>
#include <ctime>

namespace noteapp::core {

namespace {

constexpr std::string_view kStampPrefix = "note-";
constexpr std::string_view kStampFormat = "%Y%m%d-%H%M%S";

bool is_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// Boşluk dahil kırpılacak baştaki işaretler: markdown başlık/liste/alıntı belirteçleri.
bool is_leading_marker(char c) {
    return c == '#' || c == '-' || c == '*' || c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string timestamp_name(std::chrono::system_clock::time_point now) {
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::array<char, 32> buffer{};
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), kStampFormat.data(), &local);
    return std::string(kStampPrefix) + std::string(buffer.data(), length);
}

}  // namespace

std::optional<std::u32string> decode_utf8(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t extra = 0;
        char32_t cp = 0;
        char32_t min_cp = 0;  // fazla uzun kodlamayı yakalamak için alt sınır
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1, cp = lead & 0x1Fu, min_cp = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2, cp = lead & 0x0Fu, min_cp = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3, cp = lead & 0x07u, min_cp = 0x10000;
        } else {
            return std::nullopt;
        }
        if (i + extra >= utf8.size()) {
            return std::nullopt;  // kesik dizi
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            if (!is_continuation(next))
                return std::nullopt;
            cp = (cp << 6) | (next & 0x3Fu);
        }
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        if (cp < min_cp || cp > 0x10FFFF || surrogate)
            return std::nullopt;
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::string encode_utf8(std::u32string_view codepoints) {
    std::string out;
    out.reserve(codepoints.size());
    for (const char32_t cp : codepoints) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool is_valid_utf8(std::string_view utf8) {
    return decode_utf8(utf8).has_value();
}

std::string transliterate_turkish(std::string_view utf8) {
    auto decoded = decode_utf8(utf8);
    if (!decoded)
        return std::string(utf8);
    for (char32_t& cp : *decoded) {
        switch (cp) {
        case U'ç':
            cp = U'c';
            break;
        case U'Ç':
            cp = U'C';
            break;
        case U'ğ':
            cp = U'g';
            break;
        case U'Ğ':
            cp = U'G';
            break;
        case U'ı':
            cp = U'i';
            break;
        case U'İ':
            cp = U'i';
            break;
        case U'ö':
            cp = U'o';
            break;
        case U'Ö':
            cp = U'O';
            break;
        case U'ş':
            cp = U's';
            break;
        case U'Ş':
            cp = U'S';
            break;
        case U'ü':
            cp = U'u';
            break;
        case U'Ü':
            cp = U'U';
            break;
        default:
            break;
        }
    }
    return encode_utf8(*decoded);
}

std::string slugify(std::string_view first_line, std::chrono::system_clock::time_point now,
                    std::size_t max_length) {
    while (!first_line.empty() && is_leading_marker(first_line.front()))
        first_line.remove_prefix(1);

    // Geçersiz UTF-8 dönüştürülmeden gelir; ASCII olmayan baytlar zaten ayırıcı olacağından zararsız.
    const std::string ascii = transliterate_turkish(first_line);

    std::string slug;
    for (const char raw : ascii) {
        const char c = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            slug.push_back(c);
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();

    if (slug.size() > max_length) {
        // rfind(pos) pos dahil arar: tam max_length'teki tire de temiz bir kesim noktasıdır.
        const std::size_t dash = slug.rfind('-', max_length);
        slug.resize(dash == std::string::npos ? max_length : dash);
        while (!slug.empty() && slug.back() == '-')
            slug.pop_back();
    }
    return slug.empty() ? timestamp_name(now) : slug;
}

std::string normalize_line_endings(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;  // CR atlanır, LF kalır
        out += text[i];
    }
    return out;
}

std::string first_non_empty_line(std::string_view content) {
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos)
            end = content.size();
        const std::string_view line = content.substr(start, end - start);
        if (line.find_first_not_of(" \t\r") != std::string_view::npos)
            return std::string(line);
        start = end + 1;
    }
    return {};
}

}  // namespace noteapp::core
