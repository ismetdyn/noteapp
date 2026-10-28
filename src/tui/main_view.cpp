#include "tui/main_view.hpp"

namespace noteapp::tui {

namespace {

// Sol panel genişliği; SPEC-tui'deki 60 sütunluk en küçük pencerede önizlemeye yer bırakır.
constexpr int kNoteListWidth = 32;

}  // namespace

ftxui::Element main_view() {
    using namespace ftxui;

    // TODO(T09): i18n — çubuk metinleri katalogdan gelecek; örnek notlar T18'de NoteStore'dan gelecek.
    const Element query_bar =
        hbox({text("> "), text("sorgu") | dim, filler(), text("[ad] 3/3   senkron kapalı")});
    const Element note_list = vbox({
        text("▸ iş/toplantı-notları.md") | inverted,
        text("  kişisel/yapılacaklar.md"),
        text("  parçacıklar/sıralama.java"),
    });
    const Element preview = vbox({
        text("# Toplantı notları") | bold,
        text("- bütçe onayı"),
        text("çğıİöşü ÇĞIÖŞÜ"),
    });
    const Element key_hints = text("q/Esc çık  Ctrl-C çık") | dim;

    return vbox({
               query_bar,
               separator(),
               hbox({note_list | size(WIDTH, EQUAL, kNoteListWidth), separator(), preview | flex}) | flex,
               separator(),
               key_hints,
           }) |
           border;
}

}  // namespace noteapp::tui
