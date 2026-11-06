#pragma once

#include "app/commands.hpp"
#include "edit/key.hpp"

#include <string_view>

namespace noteapp::tui {

enum class Intent;
class TuiState;

// Satırın geçerli olduğu görünüm: aynı harf görünüme göre başka iştir (`n`, `E`). Görünümler ortak kaydın
// bağlamlarıdır; REVIEW önizlemedir.
inline constexpr unsigned kInList = app::kInList;
inline constexpr unsigned kInReview = app::kInPreview;
inline constexpr unsigned kInTrash = app::kInTrash;
// Editörde her tuş metne gider (TuiState::handle); bu bağlamdaki satır editörün kendi tuşunu tabloya ve
// palete taşır, palet başka görünümde nedenini söyler.
inline constexpr unsigned kInEditor = app::kInEditor;
inline constexpr unsigned kInBrowse = kInList | kInReview;
inline constexpr unsigned kInListing = kInList | kInTrash;
inline constexpr unsigned kEverywhere = kInList | kInReview | kInTrash;

// Komutun durumdan istediği (app/commands.hpp). Yank ve Sync tuşta engel değildir: tuş çalışır, nedeni alt
// satırda söylenir; palet aynı nedeni önceden gösterir.
using app::CommandNeeds;

// Tek dağıtım tablosu: Liste, REVIEW ve çöp tuşları (TuiState::handle_focused), palet ve testlerle F1 buradan
// okur. Aynı `id` birden çok satırda olabilir: görünüm başına bir eylem, palet tek satır gösterir. Kimlik ve
// etiket ortak kayıttandır: kayıtta olmayan kimlik ya da kaydın bağlamları dışındaki görünüm derleme
// hatasıdır.
struct Command {
    consteval Command(std::string_view command_id, std::string_view shown_keys, unsigned views,
                      Intent (*action)(TuiState&), CommandNeeds guard = CommandNeeds::Nothing,
                      Key bound = Key::Char, std::string_view second_key = {})
        : id(command_id), label_key(registered(command_id, views).label_key), keys(shown_keys),
          contexts(views), run(action), needs(guard), key(bound), alias(second_key) {}

    std::string_view id;    // kararlı İngilizce ad (app::kCommandRegistry)
    const char* label_key;  // i18n, kayıttan
    std::string_view keys;  // gösterilen tuş; `key` Char ise aynı zamanda bağ. Boş: yalnız palette
    unsigned contexts;      // kInList | kInReview | kInTrash
    Intent (*run)(TuiState&);
    CommandNeeds needs;
    Key key;                 // harf olmayan bağ (`F2`, `PgUp`)
    std::string_view alias;  // ikinci harf (`?`, `,`, `T`); yanında gösterilir

private:
    static consteval const app::CommandSpec& registered(std::string_view command_id, unsigned views) {
        if (!app::has_command(command_id))
            throw "TUI command id is not in app::kCommandRegistry";
        const app::CommandSpec& spec = *app::find_command(command_id);
        if ((views & ~spec.contexts) != 0)
            throw "TUI view is outside the registry contexts of the command";
        return spec;
    }
};

// Paletin bir satırı; `reason` doluysa i18n anahtarı: soluk çizilir, çalıştırılamaz.
struct PaletteRow {
    const Command* command;
    const char* reason;
};

}  // namespace noteapp::tui
