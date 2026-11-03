#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::sync {

// libgit2 üç yönlü metin birleştirmesi: çakışmasızsa sonuç baytları (CRLF, BOM aynen), aksi halde boş. Girdi
// sırası sonucu belirler: çağıran deterministik verir. max_input_bytes (limits.sync.merge_input_bytes,
// sınırsız SIZE_MAX) aşan girdide boş: çakışma kopya not olur.
std::optional<std::string> merge_text(std::string_view base, std::string_view ours, std::string_view theirs,
                                      std::size_t max_input_bytes);

}  // namespace noteapp::sync
