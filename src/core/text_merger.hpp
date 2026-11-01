#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::core {

// Üç yönlü metin birleştirme (taban, bizim, onların): temizse sonuç, çakışma ya da hata varsa boş. core
// libgit2'ye bağlanamaz: sync verir (sync/text_merge.hpp). Verilmezse her gövde çakışması kopya olur.
using TextMerger =
    std::function<std::optional<std::string>(std::string_view, std::string_view, std::string_view)>;

}  // namespace noteapp::core
