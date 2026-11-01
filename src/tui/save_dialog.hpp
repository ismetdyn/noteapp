#pragma once

#include "core/config.hpp"
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
#include <utility>
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
    // Taşıma kipi (Ctrl-R): ad/tip slug ve öneriden değil mevcut notun kendisinden gelir (move_dialog_init).
    bool move = false;
    std::string name;
    std::string type;
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
    bool is_move() const { return init_.move; }
    // Başlık katalog anahtarı: kaydetme / taşıma.
    std::string title() const;

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

// Mevcut notu taşıma diyaloğu girdisi: ad = dosya adı (uzantısız), klasör = üst klasör, tip = uzantı.
SaveDialogInit move_dialog_init(const core::NotePath& current, std::vector<std::string> folders,
                                std::vector<std::string> types, std::size_t slug_max_length);

// Taşıma/yeniden adlandırma/tip değiştirme. Hedef yol değişmediyse hiçbir şey yapmaz (kaynağı döner);
// hedef varsa NoteStore::move AlreadyExists ile reddeder (ezme yok).
std::expected<core::NotePath, core::Error> move_note(core::NoteStore& store, const core::NotePath& from,
                                                     const SaveRequest& request);

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

// `a` ile listenin başında açılan ad satırı. Açıkken tüm tuşlar buraya gelir; yazılan harfler kısayol olarak
// çalışmaz. FTXUI'ye bağlı değildir.
class NewRowState {
public:
    enum class Outcome { Editing, Cancelled, Submit };

    Outcome handle(Key key, std::string_view text = {});

    const std::string& text() const { return text_; }
    const std::string& error() const { return error_; }
    // Geçersiz ya da dolu ad: satır açık kalır, yazılan metin kaybolmaz.
    void set_error(std::string error) { error_ = std::move(error); }

private:
    std::string text_;
    std::string error_;
};

// `F2` ile açılan yol girdisi: `klasör/ad.uzantı` ön dolu, imleç adın sonunda (uzantıdan önce). Diyalogsuz
// hızlı yeniden adlandırma; FTXUI'ye bağlı değildir.
class RenameState {
public:
    enum class Outcome { Editing, Cancelled, Submit };

    explicit RenameState(core::NotePath from);

    Outcome handle(Key key, std::string_view text = {});

    const core::NotePath& from() const { return from_; }
    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }  // bayt ofseti, kod noktası başında
    const std::string& error() const { return error_; }
    void set_error(std::string error) { error_ = std::move(error); }

private:
    core::NotePath from_;
    std::string text_;
    std::size_t cursor_ = 0;
    std::string error_;
};

// `a`: ad satırındaki metinden boş not; uzantı yazılmadıysa default_type, klasör yazılmadıysa default_folder.
// Yazılan ad doluysa -2 eklenmez, hata döner: satır açık kalır.
std::expected<core::CreatedNote, core::Error> create_named_note(core::NoteStore& store, std::string_view name,
                                                                std::string_view default_folder,
                                                                std::string_view default_type,
                                                                std::size_t slug_max_length);

}  // namespace noteapp::tui
