#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace noteapp::sync {

// libgit2 üç yönlü metin birleştirmesi: çakışmasızsa sonuç baytları (CRLF, BOM aynen), aksi halde boş. Girdi
// sırası sonucu belirler: çağıran deterministik verir.
std::optional<std::string> merge_text(std::string_view base, std::string_view ours, std::string_view theirs);

}  // namespace noteapp::sync
