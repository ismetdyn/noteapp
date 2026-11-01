#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::gui {

struct SaveDetails {
    std::string name;    // uzantısız ad
    std::string folder;  // "" = kök
    std::string type;    // uzantı, noktasız
};

// Kaydet/taşı diyaloğunun girdisi; testlerde karar enjekte edilirken de aynı yapı kullanılır.
struct SaveDialogRequest {
    enum class Mode { Create, Move };
    Mode mode = Mode::Create;
    SaveDetails initial;
    std::vector<std::string> folders;
    std::vector<std::string> types;
    std::string hint;          // "öneri: java %82"; taşıma kipinde boş
    bool folder_only = false;  // Ctrl+M: ad ve tip kilitli, yalnız klasör seçilir
};

// Ad / klasör / tip formu: çizim view'dadır, kurallar burada. Doğrulama hatası formu kapatmaz.
struct SaveForm {
    explicit SaveForm(const SaveDialogRequest& request) : fields(request.initial) {}

    SaveDetails fields;
    std::string error;

    // Boşlukları kırpar, elle yazılan ".txt"nin noktasını atar; adda yazılan uzantı tipin yerine geçer.
    SaveDetails details() const;
    bool try_accept();  // geçersizse error dolar ve false döner
};

enum class ConflictChoice { Reload, Overwrite, SaveCopy, Cancel };
enum class CloseChoice { Save, Discard, Cancel };

// Düğmeli soru: varsayılan (Enter) ve Esc düğmesi veri olarak tutulur, böylece çizimsiz test edilir.
template <typename Choice> struct ChoiceBox {
    std::string title;
    std::string message;
    std::vector<std::pair<std::string, Choice>> buttons;
    std::size_t default_button = 0;
    std::size_t escape_button = 0;
};

// Varsayılan düğme "benimkini kopya olarak kaydet" (veri kaybettirmeyen seçenek), Esc = vazgeç.
ChoiceBox<ConflictChoice> make_conflict_box(const std::string& path);
ChoiceBox<CloseChoice> make_close_box();
ChoiceBox<CloseChoice> make_close_settings_box();
// Silme onayı: varsayılan ve Esc düğmesi "Vazgeç" (geri alınamaz işlem; Enter'a basmak silmesin).
ChoiceBox<bool> make_delete_box(const std::string& message);

}  // namespace noteapp::gui
