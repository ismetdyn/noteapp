#pragma once

#include "tui/key.hpp"

#include <string_view>

namespace noteapp::tui {

enum class Intent;
class TuiState;

// Satırın geçerli olduğu görünüm: aynı harf görünüme göre başka iştir (`n`, `E`).
inline constexpr unsigned kInList = 1U;
inline constexpr unsigned kInReview = 2U;
inline constexpr unsigned kInTrash = 4U;
inline constexpr unsigned kInBrowse = kInList | kInReview;
inline constexpr unsigned kInListing = kInList | kInTrash;
inline constexpr unsigned kEverywhere = kInList | kInReview | kInTrash;

// Komutun durumdan istediği. Yank ve Sync tuşta engel değildir: tuş çalışır, nedeni alt satırda söylenir;
// palet aynı nedeni önceden gösterir.
enum class CommandNeeds { Nothing, Note, Row, Links, FolderView, Yank, Sync };

// Tek dağıtım tablosu: Liste, REVIEW ve çöp tuşları (TuiState::handle_focused), palet ve testlerle F1 buradan
// okur. Aynı `id` birden çok satırda olabilir: görünüm başına bir eylem, palet tek satır gösterir.
struct Command {
    std::string_view id;    // kararlı İngilizce ad
    const char* label_key;  // i18n
    std::string_view keys;  // gösterilen tuş; `key` Char ise aynı zamanda bağ. Boş: yalnız palette
    unsigned contexts;      // kInList | kInReview | kInTrash
    Intent (*run)(TuiState&);
    CommandNeeds needs = CommandNeeds::Nothing;
    Key key = Key::Char;          // harf olmayan bağ (`F2`, `PgUp`)
    std::string_view alias = {};  // ikinci harf (`?`, `,`, `T`); yanında gösterilir
};

// Paletin bir satırı; `reason` doluysa i18n anahtarı: soluk çizilir, çalıştırılamaz.
struct PaletteRow {
    const Command* command;
    const char* reason;
};

}  // namespace noteapp::tui
