#include "core/note_path.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace noteapp::core {

namespace {

constexpr std::size_t kMaxComponentBytes = 255;  // NTFS/ext4 ortak bileşen sınırı
constexpr std::string_view kReservedChars = "<>:\"|?*";
constexpr std::array<std::string_view, 4> kReservedDevices = {"CON", "PRN", "AUX", "NUL"};
constexpr std::array<std::string_view, 3> kSuperscriptDigits = {"\xC2\xB9", "\xC2\xB2", "\xC2\xB3"};  // ¹ ² ³

Error invalid_path(std::string_view reason, std::string_view path) {
    return Error{Error::Code::InvalidPath,
                 "error.invalid_path",
                 {{"reason", std::string(reason)}, {"path", std::string(path)}}};
}

bool is_ascii_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_control(char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
}

// "con", "CON.txt", "Lpt1.md", "COM0", "lpt²": ilk noktaya kadarki gövde ayrılmış aygıt adıysa true.
// Gövdedeki sondaki boşluk Windows'ta yok sayıldığından ("con .txt") onu da yakalar. Windows COM/LPT'nin
// 0'ını ve üst simge ¹ ² ³ (U+00B9, U+00B2, U+00B3) biçimlerini de aygıt sayar.
bool is_reserved_device(std::string_view component) {
    std::string stem(component.substr(0, component.find('.')));
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    for (char& c : stem)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto device : kReservedDevices) {
        if (stem == device)
            return true;
    }
    if (stem == "CONIN$" || stem == "CONOUT$")  // Win32 konsol aygıtları: CreateFile yolu aygıta açar
        return true;
    if (!stem.starts_with("COM") && !stem.starts_with("LPT"))
        return false;
    const std::string_view digit = std::string_view(stem).substr(3);
    const bool ascii_digit = digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9';
    return ascii_digit || std::ranges::find(kSuperscriptDigits, digit) != kSuperscriptDigits.end();
}

// Tek bir bileşenin kuralları; ihlalde `reason` değerini, yoksa boş string_view döner.
std::string_view component_violation(std::string_view component) {
    if (component.empty())
        return "empty_component";
    if (component == ".")
        return "dot";
    if (component == "..")
        return "dot_dot";
    if (component.front() == '.')
        return "hidden";
    for (const char c : component) {
        if (is_control(c))
            return "control_char";
        if (kReservedChars.find(c) != std::string_view::npos)
            return "invalid_char";
    }
    if (is_reserved_device(component))
        return "reserved_name";
    if (component.back() == ' ')
        return "trailing_space";
    if (component.back() == '.')
        return "trailing_dot";
    if (component.size() > kMaxComponentBytes)
        return "too_long";
    return {};
}

}  // namespace

std::expected<NotePath, Error> NotePath::parse(std::string_view relative) {
    // UTF-8 geçerliliği en başta: sonraki bayt düzeyindeki denetimler çok baytlı karakterleri
    // yanlış yorumlayamaz (UTF-8'de ASCII baytlar yalnızca ASCII karakterlerdir).
    if (relative.empty())
        return std::unexpected(invalid_path("empty", relative));
    if (!is_valid_utf8(relative))
        return std::unexpected(invalid_path("invalid_utf8", relative));

    // Ayırıcı yalnız '/'; ters bölü Windows'ta ayırıcı olduğundan hiçbir biçimde kabul edilmez.
    if (relative.starts_with("\\\\"))
        return std::unexpected(invalid_path("unc", relative));
    if (relative.find('\\') != std::string_view::npos) {
        return std::unexpected(invalid_path("backslash", relative));
    }
    if (relative.front() == '/')
        return std::unexpected(invalid_path("absolute", relative));
    if (relative.size() >= 2 && is_ascii_alpha(relative[0]) && relative[1] == ':') {
        return std::unexpected(invalid_path("drive_letter", relative));
    }

    std::size_t start = 0;
    while (true) {
        const std::size_t end = relative.find('/', start);
        const std::string_view component = relative.substr(start, end - start);
        if (const auto reason = component_violation(component); !reason.empty()) {
            return std::unexpected(invalid_path(reason, relative));
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    // Dar `char` kurucusu Windows'ta ANSI kod sayfası varsayar; UTF-8'i char8_t olarak vermek şart.
    const std::u8string utf8(reinterpret_cast<const char8_t*>(relative.data()), relative.size());
    return NotePath(std::filesystem::path(utf8));
}

std::string NotePath::string() const {
    const std::u8string utf8 = value_.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace noteapp::core
