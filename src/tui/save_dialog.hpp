#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"
#include "tui/tui_state.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Diyaloğu kuran girdiler. Hepsi dışarıdan verilir: model G/Ç yapmaz, saattten bağımsızdır (test edilebilir).
struct SaveDialogInit {
    std::string content;               // editörden dönen metin; ad slug'ı ilk dolu satırdan
    std::string folder;                // ön dolu klasör (bkz. initial_folder)
    std::vector<std::string> folders;  // NoteStore::folders(): öneri havuzu
    std::vector<std::string> types;    // config.types: ←/→ döngüsü
    detect::Suggestion suggestion;
    std::size_t slug_max_length = core::kDefaultSlugMaxLength;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
};

// Kaydet diyaloğunun onayladığı değerler; NoteStore::create'e (yeni not) ya da move'a (T22) girdi olur.
// `name` boşsa not içeriğinden slug türetilir (CLI ile aynı kural).
struct SaveRequest {
    std::string folder;
    std::string name;
    std::string type;
};

// Ad / klasör / tip diyaloğunun saf modeli (SPEC-tui "Editör ve kaydet diyaloğu"). FTXUI'ye bağlı değildir.
class SaveDialogState {
public:
    enum class Field { Name, Folder, Type };
    // Submit: istek hazır (request()). Discard: kullanıcı notu atmayı onayladı.
    enum class Outcome { Editing, Submit, Discard };

    // Klasör önerisi en çok bu kadar satır gösterilir (SPEC-tui).
    static constexpr std::size_t kMaxSuggestions = 5;

    explicit SaveDialogState(SaveDialogInit init);

    Outcome handle(Key key, std::string_view text = {});

    // Kaydetme başarısız olunca çağıran hatayı satır içi gösterir; diyalog açık kalır.
    void set_error(std::string message) { error_ = std::move(message); }

    const std::string& name() const { return name_; }
    const std::string& folder() const { return folder_; }
    const std::string& type() const { return type_; }
    // "öneri: java %82"; yalnız tip hâlâ önerilen değerse ve algılama gerçekten yapıldıysa dolu.
    std::string type_label() const;
    Field focus() const { return focus_; }
    const std::vector<std::string>& suggestions() const { return suggestions_; }
    // Vurgulu öneri; yoksa -1.
    int selected_suggestion() const { return selected_suggestion_; }
    const std::string& error() const { return error_; }
    bool confirming_discard() const { return confirming_discard_; }
    // Onay penceresinde "At" seçili mi (varsayılan: hayır = "Vazgeç").
    bool discard_selected() const { return discard_selected_; }
    const SaveRequest& request() const { return request_; }

private:
    Outcome handle_confirm(Key key);
    void edit_text(Key key, std::string_view text);
    void move_focus(int delta);
    void cycle_type(int delta);
    void refresh_suggestions();
    void accept_suggestion();
    Outcome submit();
    std::string& field_text();

    SaveDialogInit init_;
    std::string name_;
    std::string folder_;
    std::string type_;
    bool type_typing_ = false;  // true: yazılan karakter eklenir; false: ilk karakter tipi değiştirir
    Field focus_ = Field::Name;
    std::vector<std::string> suggestions_;
    int selected_suggestion_ = -1;
    std::string error_;
    bool confirming_discard_ = false;
    bool discard_selected_ = false;
    SaveRequest request_;
};

// Klasör ön değeri: seçili notun klasörü varsa o, yoksa config.default_folder.
std::string initial_folder(const core::NotePath* selected, std::string_view default_folder);

// Yalnız boşluk/satır sonundan oluşan metin "boş" sayılır (CLI ile aynı).
bool is_blank(std::string_view text);

// Geçici dosyadaki içeriği not olarak kaydeder. Başarıda geçici dosya silinir; hatada DOKUNULMAZ (veri
// kaybı olmasın) ve çağıran yolu kullanıcıya bildirir.
std::expected<core::NotePath, core::Error> save_new_note(core::NoteStore& store,
                                                         const std::filesystem::path& temp_file,
                                                         const SaveRequest& request,
                                                         std::string_view content);

}  // namespace noteapp::tui
