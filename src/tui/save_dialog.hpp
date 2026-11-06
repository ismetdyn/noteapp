#pragma once

#include "app/item_ops.hpp"
#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"
#include "search/fuzzy.hpp"
#include "tui/tui_state.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Diyaloğu kuran girdiler. Hepsi dışarıdan verilir: model G/Ç yapmaz, saattten bağımsızdır (test edilebilir).
struct SaveDialogInit {
    // Klasör önerisi en çok bu kadar satır gösterilir (SPEC-tui); limits.tui.save_suggestions varsayılanı.
    static constexpr std::size_t kDefaultSuggestions = 5;
    std::string content;               // editörden dönen metin; ad slug'ı ilk dolu satırdan
    std::string folder;                // ön dolu klasör (bkz. initial_folder)
    std::vector<std::string> folders;  // NoteStore::folders(): öneri havuzu
    std::vector<std::string> types;    // config.types: ←/→ döngüsü
    detect::Suggestion suggestion{};
    std::size_t slug_max_length = core::kDefaultSlugMaxLength;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    // Taşıma kipi (Ctrl-R): ad/tip slug ve öneriden değil mevcut notun kendisinden gelir (move_dialog_init).
    bool move = false;
    std::string name;
    std::string type;
    // limits.tui.save_suggestions ve limits.search.fuzzy_candidate_chars (sınırsız SIZE_MAX).
    std::size_t max_suggestions = SaveDialogInit::kDefaultSuggestions;
    std::size_t fuzzy_candidate_chars = search::kDefaultFuzzyCandidateChars;
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

    explicit NewRowState(SharedInputSources sources = {}) { line_.set_sources(std::move(sources)); }
    // `folder.new`: `prefix` + `/` ön dolu, imleç sondaki `/`ten önce; yazılan ad klasör olur (names_folder).
    struct FolderName {};
    NewRowState(FolderName, std::string prefix, SharedInputSources sources)
        : NewRowState(std::move(sources)) {
        const std::size_t cursor = prefix.size();
        line_.set_text(std::move(prefix) + "/", cursor);
    }

    Outcome handle(Key key, std::string_view text = {});

    const std::string& text() const { return line_.text(); }
    const InputLine& line() const { return line_; }
    std::string ghost() const { return line_.ghost(); }
    const std::string& error() const { return error_; }
    // Geçersiz ya da dolu ad: satır açık kalır, yazılan metin kaybolmaz.
    void set_error(std::string error) { error_ = std::move(error); }

private:
    InputLine line_;
    std::string error_;
};

// `F2` ile açılan yol girdisi: `klasör/ad.uzantı` ön dolu, imleç adın sonunda (uzantıdan önce). Diyalogsuz
// hızlı yeniden adlandırma; FTXUI'ye bağlı değildir.
class RenameState {
public:
    enum class Outcome { Editing, Cancelled, Submit };

    // `base`: girdi bu klasöre göredir, ön dolu metin ondan kısalır. Klasör adında nokta uzantı değildir.
    explicit RenameState(core::NotePath from, SharedInputSources sources = {}, std::string base = {},
                         bool is_folder = false);
    // `m`: yalnız hedef klasör sorulur; `target()` klasördür, öğeler boş olamaz.
    RenameState(std::vector<ListItem> items, std::string folder, SharedInputSources sources);
    // `L`: `from` notuna ilişkilendirilecek notun yolu; girdi boş başlar.
    struct LinkTarget {};
    RenameState(LinkTarget, core::NotePath from, SharedInputSources sources);
    // PDF dışa aktarma: `from` notunun çıktı yolu; `file_name` ile dolar, imleç uzantıdan önce. Göreli yol
    // çalışma dizinine göredir (CLI `note export --pdf` gibi).
    struct ExportTarget {};
    RenameState(ExportTarget, core::NotePath from, std::string file_name);
    // Palet `import`: içe alınacak dosya ya da klasörün yolu (göreli yol çalışma dizinine göre); girdi boş
    // başlar, `folder` hedeftir. `from()` yoktur.
    struct ImportSource {};
    RenameState(ImportSource, std::string folder);
    // F1 adlı ilişki: `L`'nin ikinci adımı ve harita `C` `r`, `from` ile `to` arasındaki ilişkinin adı (boş =
    // adsız). Tab var olan adları tamamlar; Ctrl+D (Directed) "yönlü"yü değiştirir, değiştirilmedikçe yazılan
    // ad var olan bir adsa onun varsayılanıdır (app::default_directed), yeni ad yönsüz. `renaming` doluysa o
    // ilişkinin adı değişir: metin `current` ile dolar, yön sorulmaz.
    struct RelationName {};
    using SharedRelationNames = std::shared_ptr<const std::vector<core::RelationName>>;
    RenameState(RelationName, core::NotePath from, core::NotePath to, SharedRelationNames names,
                std::optional<core::RelationId> renaming = std::nullopt, std::string current = {});

    Outcome handle(Key key, std::string_view text = {});

    const core::NotePath& from() const { return from_.value(); }  // içe almada yok: çağrılmaz
    const std::string& text() const { return line_.text(); }
    // Girdinin tam yolu: `base` + yazılan; klasörde sondaki `/` düşer.
    std::string target() const;
    bool is_folder() const { return is_folder_; }
    bool is_move_to() const { return !items_.empty(); }
    bool is_link() const { return is_link_; }
    bool is_export() const { return is_export_; }
    bool is_import() const { return is_import_; }
    bool is_relation() const { return relation_to_.has_value(); }
    const core::NotePath& relation_to() const { return relation_to_.value(); }  // yalnız ad satırında
    const std::optional<core::RelationId>& renaming() const { return renaming_; }
    bool directed() const;
    const std::string& import_folder() const { return base_; }
    const std::vector<ListItem>& items() const { return items_; }
    void set_items(std::vector<ListItem> items) { items_ = std::move(items); }
    std::size_t cursor() const { return line_.cursor(); }  // bayt ofseti, kod noktası başında
    const InputLine& line() const { return line_; }
    std::string ghost() const { return line_.ghost(); }
    const std::string& error() const { return error_; }
    void set_error(std::string error) { error_ = std::move(error); }

private:
    std::optional<core::NotePath> from_;
    std::string base_;
    bool is_folder_ = false;
    bool is_link_ = false;
    bool is_export_ = false;
    bool is_import_ = false;
    std::optional<core::NotePath> relation_to_;
    std::optional<core::RelationId> renaming_;
    SharedRelationNames relation_names_;
    std::optional<bool> directed_choice_;  // Ctrl+D ile seçildi; yoksa adın varsayılanı
    std::vector<ListItem> items_;
    InputLine line_;
    std::string error_;
};

// `a`: ad satırı kuralı GUI ile ortak (app/item_ops.hpp); bu adlar mevcut çağıranlar ve testler için kalır.
using app::create_named_note;
using app::names_folder;

}  // namespace noteapp::tui
