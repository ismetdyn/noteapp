#pragma once

#include "core/i18n.hpp"

#include <string>
#include <string_view>

namespace noteapp::cli {

// Silme onayı: kırpılmış yanıt TEK karakter olmalı ve yerelleştirilmiş "evet" harflerinden biri olmalı
// (tr: e/E, en: y/Y; harfler katalogda, kodda değil). Başka her şey ("evet", boş, y/n karışıklığı)
// onay sayılmaz: şüphede silinmez. Geri dönüşü zor diğer onaylar da (sync init'te origin değiştirme) bunu
// kullanır.
inline bool is_delete_confirmed(std::string_view answer) {
    constexpr std::string_view kBlanks = " \t\r\n";
    const std::size_t first = answer.find_first_not_of(kBlanks);
    if (first == std::string_view::npos)
        return false;
    const std::string_view trimmed = answer.substr(first, answer.find_last_not_of(kBlanks) - first + 1);
    return trimmed.size() == 1 &&
           core::tr("cli.confirm_yes_letters").find(trimmed.front()) != std::string::npos;
}

}  // namespace noteapp::cli
