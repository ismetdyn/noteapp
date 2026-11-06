#include "tui/save_dialog.hpp"

#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/editor_session.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::string_view kBlankChars = " \t\r\n";
constexpr char kDelete = 0x7f;

// UTF-8 devam baytı (10xxxxxx): Backspace bir kod noktasının tamamını silsin.
bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

void pop_code_point(std::string& s) {
    while (s.size() > 1 && is_continuation(s.back()))
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

// Kontrol karakterleri alana girmez (terminal olaylarından sızabilir).
bool is_printable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20 && text.front() != kDelete;
}

// NotePath'in hata biçimiyle aynı: satır içi hata mesajı format_error ile üretilir.
core::Error invalid(std::string_view reason, std::string_view value) {
    return core::Error{core::Error::Code::InvalidPath,
                       "error.invalid_path",
                       {{"reason", std::string(reason)}, {"path", std::string(value)}}};
}

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool has_separator(std::string_view s) {
    return s.find_first_of("/\\") != std::string_view::npos;
}

// NoteStore::create'in reddedeceği her şeyi diyalogda yakalar: kullanıcı hatayı kaydetmeden önce görür.
// Adda yazılan uzantı Tip alanını geçer (SaveDialogState::submit): o zaman Tip alanı denetlenmez.
std::optional<core::Error> validate(const std::string& folder, const std::string& name,
                                    const std::string& type) {
    const bool typed_extension = !core::split_typed_name(name).extension.empty();
    if (!typed_extension && (type.empty() || type.find('.') != std::string::npos || has_separator(type)))
        return invalid("invalid_type", type);
    if (has_separator(name))
        return invalid("invalid_name", name);
    if (!name.empty())
        if (auto path = core::NotePath::parse(typed_extension ? name : name + "." + type); !path)
            return path.error();
    if (!folder.empty())
        if (auto path = core::NotePath::parse(folder); !path)
            return path.error();
    return std::nullopt;
}

}  // namespace

SaveDialogState::SaveDialogState(SaveDialogInit init)
    : init_(std::move(init)),
      name_(init_.move ? init_.name
                       : core::slugify(core::note_name_source(init_.content, init_.suggestion.type),
                                       init_.now, init_.slug_max_length)),
      folder_(init_.folder), type_(init_.move ? init_.type : init_.suggestion.type) {
    refresh_suggestions();
}

std::string SaveDialogState::title() const {
    return core::tr(init_.move ? "tui.move_title" : "tui.save_title");
}

std::string SaveDialogState::type_label() const {
    if (!init_.suggestion.detected || type_ != init_.suggestion.type)
        return {};
    const long percent = std::lround(init_.suggestion.confidence * 100.0);
    return core::tr("tui.type_suggested", {{"type", type_}, {"percent", std::to_string(percent)}});
}

std::string& SaveDialogState::field_text() {
    switch (focus_) {
    case Field::Name:
        return name_;
    case Field::Folder:
        return folder_;
    case Field::Type:
        return type_;
    }
    return name_;
}

void SaveDialogState::refresh_suggestions() {
    std::vector<std::pair<double, const std::string*>> scored;
    for (const std::string& candidate : init_.folders)
        if (const auto match = search::fuzzy_match(folder_, candidate, init_.fuzzy_candidate_chars))
            scored.emplace_back(match->score, &candidate);
    std::ranges::stable_sort(scored, [](const auto& a, const auto& b) { return a.first > b.first; });
    suggestions_.clear();
    for (std::size_t i = 0; i < scored.size() && i < init_.max_suggestions; ++i)
        suggestions_.push_back(*scored[i].second);
    selected_suggestion_ = -1;
}

void SaveDialogState::accept_suggestion() {
    if (suggestions_.empty())
        return;
    folder_ = suggestions_[static_cast<std::size_t>(std::max(selected_suggestion_, 0))];
    refresh_suggestions();
}

void SaveDialogState::edit_text(Key key, std::string_view text) {
    error_.clear();
    std::string& target = field_text();
    if (focus_ == Field::Type && !type_typing_) {
        // Önerilen/seçilen tip ilk tuşta tümden değişir; sonrası ekleme.
        if (key == Key::Char && !is_printable(text))
            return;
        type_.clear();
        type_typing_ = true;
    }
    if (key == Key::Backspace) {
        pop_code_point(target);
    } else if (is_printable(text)) {
        if (focus_ == Field::Type && target.empty() && text == ".")
            return;  // ".xml" yazan kullanıcı için baştaki nokta atılır
        target.append(text);
    }
    if (focus_ == Field::Folder)
        refresh_suggestions();
}

void SaveDialogState::move_focus(int delta) {
    constexpr int kFields = 3;
    focus_ = static_cast<Field>((static_cast<int>(focus_) + delta + kFields) % kFields);
}

void SaveDialogState::cycle_type(int delta) {
    if (init_.types.empty())
        return;
    error_.clear();
    const auto count = static_cast<int>(init_.types.size());
    const auto it = std::ranges::find(init_.types, type_);
    int next = 0;
    if (it == init_.types.end())
        next = delta > 0 ? 0 : count - 1;
    else
        next = (static_cast<int>(it - init_.types.begin()) + delta + count) % count;
    type_ = init_.types[static_cast<std::size_t>(next)];
    type_typing_ = false;
}

SaveDialogState::Outcome SaveDialogState::submit() {
    if (init_.move && name_.empty()) {
        // Taşımada içerikten slug türetilmez: boş ad notu adsız bırakırdı.
        error_ = core::format_error(invalid("invalid_name", name_));
        return Outcome::Editing;
    }
    if (const auto problem = validate(folder_, name_, type_)) {
        error_ = core::format_error(*problem);
        return Outcome::Editing;
    }
    const core::TypedName typed = core::split_typed_name(name_);
    request_ = {folder_, std::string(typed.stem),
                typed.extension.empty() ? type_ : std::string(typed.extension)};
    return Outcome::Submit;
}

SaveDialogState::Outcome SaveDialogState::handle_confirm(Key key) {
    switch (key) {
    case Key::Left:
    case Key::Right:
    case Key::Tab:
    case Key::ShiftTab:
    case Key::Up:
    case Key::Down:
        discard_selected_ = !discard_selected_;
        return Outcome::Editing;
    case Key::Enter:
        if (discard_selected_)
            return Outcome::Discard;
        confirming_discard_ = false;
        return Outcome::Editing;
    case Key::Escape:
        confirming_discard_ = false;  // Esc = vazgeç
        return Outcome::Editing;
    default:
        return Outcome::Editing;
    }
}

SaveDialogState::Outcome SaveDialogState::handle(Key key, std::string_view text) {
    if (confirming_discard_)
        return handle_confirm(key);
    switch (key) {
    case Key::Char:
    case Key::Backspace:
        edit_text(key, text);
        return Outcome::Editing;
    case Key::Tab: {
        // Klasörde tamamlanacak bir öneri varsa Tab onu tamamlar; yoksa odak ilerler.
        const bool completable =
            focus_ == Field::Folder && !suggestions_.empty() &&
            suggestions_[static_cast<std::size_t>(std::max(selected_suggestion_, 0))] != folder_;
        if (completable)
            accept_suggestion();
        else
            move_focus(1);
        return Outcome::Editing;
    }
    case Key::ShiftTab:
        move_focus(-1);
        return Outcome::Editing;
    case Key::Up:
        if (focus_ == Field::Folder && selected_suggestion_ >= 0)
            --selected_suggestion_;
        else
            move_focus(-1);
        return Outcome::Editing;
    case Key::Down:
        if (focus_ == Field::Folder && selected_suggestion_ + 1 < static_cast<int>(suggestions_.size()))
            ++selected_suggestion_;
        else
            move_focus(1);
        return Outcome::Editing;
    case Key::Left:
    case Key::Right:
        if (focus_ == Field::Type)
            cycle_type(key == Key::Right ? 1 : -1);
        return Outcome::Editing;
    case Key::Enter:
        if (focus_ == Field::Folder && selected_suggestion_ >= 0)
            accept_suggestion();  // ↓ ile seçilen öneri Enter'da uygulanır
        return submit();
    case Key::Escape:
        if (init_.move)
            return Outcome::Discard;  // taşımada kaybolacak metin yok: onaysız iptal
        confirming_discard_ = true;
        discard_selected_ = false;
        return Outcome::Editing;
    default:
        return Outcome::Editing;
    }
}

SaveDialogInit move_dialog_init(const core::NotePath& current, std::vector<std::string> folders,
                                std::vector<std::string> types, std::size_t slug_max_length) {
    const std::filesystem::path& file = current.value();
    SaveDialogInit init;
    init.move = true;
    init.name = to_utf8(file.stem());
    init.type = to_utf8(file.extension());
    if (!init.type.empty())
        init.type.erase(0, 1);  // baştaki nokta
    // `a.tar.gz`: gövde de uzantı gibi görünür; adın tamamı gösterilir, yazılmış sayılır ve aynen kalır.
    if (!core::split_typed_name(init.name).extension.empty())
        init.name = to_utf8(file.filename());
    init.folder = initial_folder(&current, "");
    init.folders = std::move(folders);
    init.types = std::move(types);
    init.suggestion = {init.type, 0.0, {}, false};  // ipucu yok: mevcut tip zaten ön dolu
    init.slug_max_length = slug_max_length;
    return init;
}

std::expected<core::NotePath, core::Error> move_note(core::NoteStore& store, const core::NotePath& from,
                                                     const SaveRequest& request) {
    std::string target = request.name + "." + request.type;
    if (!request.folder.empty())
        target = request.folder + "/" + target;
    auto to = core::NotePath::parse(target);
    if (!to)
        return std::unexpected(to.error());
    if (*to == from)
        return from;
    return store.move(from, *to);
}

std::string initial_folder(const core::NotePath* selected, std::string_view default_folder) {
    if (selected) {
        const std::string path = selected->string();
        if (const std::size_t slash = path.rfind('/'); slash != std::string::npos)
            return path.substr(0, slash);
    }
    return std::string(default_folder);
}

bool is_blank(std::string_view text) {
    return text.find_first_not_of(kBlankChars) == std::string_view::npos;
}

std::expected<core::NotePath, core::Error> save_new_note(core::NoteStore& store,
                                                         const std::filesystem::path& temp_file,
                                                         const SaveRequest& request,
                                                         std::string_view content) {
    auto created =
        store.create(core::NewNote{request.folder, request.name, request.type, std::string(content)});
    if (created) {
        std::error_code ec;
        std::filesystem::remove(temp_file, ec);  // not kaydedildi: artık geçici kopyaya gerek yok
    }
    return created;
}

namespace {

// Son yol bileşenindeki son noktanın yeri; uzantı yoksa metnin sonu.
std::size_t stem_end(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    const bool has_extension =
        dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
    return has_extension ? dot : path.size();
}

}  // namespace

RenameState::RenameState(core::NotePath from, SharedInputSources sources, std::string base, bool is_folder)
    : from_(std::move(from)), base_(std::move(base)), is_folder_(is_folder) {
    std::string path = from_.value().string();
    if (in_folder(path, base_) && !base_.empty())
        path.erase(0, base_.size() + 1);
    const std::size_t cursor = is_folder_ ? path.size() : stem_end(path);
    line_.set_text(std::move(path), cursor);
    line_.set_sources(std::move(sources));
}

RenameState::RenameState(std::vector<ListItem> items, std::string folder, SharedInputSources sources)
    : from_(items.front().path), is_folder_(true), items_(std::move(items)), line_(InputKind::Folder) {
    line_.set_text(std::move(folder));
    line_.set_sources(std::move(sources));
}

RenameState::RenameState(LinkTarget, core::NotePath from, SharedInputSources sources)
    : from_(std::move(from)), is_link_(true) {
    line_.set_sources(std::move(sources));
}

RenameState::RenameState(ExportTarget, core::NotePath from, std::string file_name)
    : from_(std::move(from)), is_export_(true) {
    const std::size_t cursor = stem_end(file_name);
    line_.set_text(std::move(file_name), cursor);
}

RenameState::RenameState(ImportSource, std::string folder) : base_(std::move(folder)), is_import_(true) {}

// Adın tamamı tek metindir (Find: sözcüklere bölünmez); adaylar adların gösterimidir.
RenameState::RenameState(RelationName, core::NotePath from, core::NotePath to, SharedRelationNames names,
                         std::optional<core::RelationId> renaming, std::string current)
    : from_(std::move(from)), relation_to_(std::move(to)), renaming_(renaming),
      relation_names_(names ? std::move(names) : std::make_shared<const std::vector<core::RelationName>>()),
      line_(InputKind::Find) {
    std::vector<std::string> displays;
    displays.reserve(relation_names_->size());
    for (const core::RelationName& name : *relation_names_)
        displays.push_back(name.display);
    line_.set_sources(word_input_sources(std::move(displays)));
    line_.set_text(std::move(current));
}

bool RenameState::directed() const {
    if (directed_choice_)
        return *directed_choice_;
    return relation_names_ && app::default_directed(*relation_names_, line_.text());
}

std::string RenameState::target() const {
    std::string_view typed = line_.text();
    if (is_folder_ && typed.ends_with('/'))
        typed.remove_suffix(1);
    return join_folder(base_, typed);
}

RenameState::Outcome RenameState::handle(Key key, std::string_view text) {
    if (key == Key::CtrlD && is_relation() && !renaming_ && !line_.popup()) {
        directed_choice_ = !directed();
        return Outcome::Editing;
    }
    const InputLine::Result result = line_.handle(key, text);
    if (clears_input_error(key, result))
        error_.clear();
    if (result != InputLine::Result::Ignored)
        return Outcome::Editing;
    if (key == Key::Enter)
        return Outcome::Submit;
    return key == Key::Escape ? Outcome::Cancelled : Outcome::Editing;
}

NewRowState::Outcome NewRowState::handle(Key key, std::string_view text) {
    const InputLine::Result result = line_.handle(key, text);
    if (clears_input_error(key, result))
        error_.clear();
    if (result != InputLine::Result::Ignored)
        return Outcome::Editing;
    if (key == Key::Enter)
        return is_blank(line_.text()) ? Outcome::Cancelled : Outcome::Submit;
    return key == Key::Escape ? Outcome::Cancelled : Outcome::Editing;
}

}  // namespace noteapp::tui
