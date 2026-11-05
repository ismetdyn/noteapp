#include "gui/save_dialog.hpp"

#include "core/i18n.hpp"
#include "core/note_path.hpp"
#include "core/text.hpp"

#include <string_view>
#include <utility>

namespace noteapp::gui {

namespace {

std::string trimmed(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(kWhitespace) - first + 1));
}

// NotePath kurallarının tamamı tek yerde (ad + klasör + tip); ilk ihlalin metni, yoksa boş.
std::string validate(const SaveDetails& d) {
    if (d.name.empty())
        return core::tr("gui.err_name_empty");
    if (d.name.find_first_of("/\\") != std::string::npos)
        return core::tr("gui.err_name_separator");
    if (d.type.empty() || d.type.find('.') != std::string::npos)
        return core::tr("gui.err_type_invalid");
    if (!d.folder.empty()) {
        if (const auto folder = core::NotePath::parse(d.folder); !folder)
            return core::format_error(folder.error());
    }
    const auto path = core::NotePath::parse((d.folder.empty() ? "" : d.folder + "/") + d.name + "." + d.type);
    return path ? std::string{} : core::format_error(path.error());
}

}  // namespace

SaveDetails SaveForm::details() const {
    std::string type = trimmed(fields.type);
    if (!type.empty() && type.front() == '.')
        type.erase(0, 1);  // ".txt" elle yazılabilir
    std::string name = trimmed(fields.name);
    // Adda yazılan uzantı Tip alanını geçer; değiştirilmez, üstüne eklenmez.
    const core::TypedName typed = core::split_typed_name(name);
    if (!typed.extension.empty()) {
        type = std::string(typed.extension);
        name = std::string(typed.stem);
    }
    return {std::move(name), trimmed(fields.folder), std::move(type)};
}

bool SaveForm::try_accept() {
    error = validate(details());
    return error.empty();
}

ChoiceBox<ConflictChoice> make_conflict_box(const std::string& path) {
    return {.title = core::tr("gui.conflict_title"),
            .message = core::tr("gui.conflict_text", {{"path", path}}),
            .buttons = {{core::tr("gui.conflict_reload"), ConflictChoice::Reload},
                        {core::tr("gui.conflict_overwrite"), ConflictChoice::Overwrite},
                        {core::tr("gui.conflict_copy"), ConflictChoice::SaveCopy},
                        {core::tr("gui.cancel"), ConflictChoice::Cancel}},
            .default_button = 2,
            .escape_button = 3,
            .danger_button = 1};  // diskteki sürümün üstüne yaz
}

ChoiceBox<CloseChoice> make_close_box() {
    return {.title = core::tr("gui.close_title"),
            .message = core::tr("gui.close_text"),
            .buttons = {{core::tr("gui.close_save"), CloseChoice::Save},
                        {core::tr("gui.close_discard"), CloseChoice::Discard},
                        {core::tr("gui.cancel"), CloseChoice::Cancel}},
            .default_button = 0,
            .escape_button = 2,
            .danger_button = 1};  // kaydetmeden kapat
}

ChoiceBox<CloseChoice> make_close_settings_box() {
    ChoiceBox<CloseChoice> box = make_close_box();
    box.title = core::tr("gui.close_settings_title");
    box.message = core::tr("gui.close_settings_text");
    box.default_button = box.escape_button;  // Vazgeç: Enter Ayarlar'da bırakır (TUI Esc sorusuyla aynı)
    return box;
}

ChoiceBox<bool> make_delete_box(const std::string& message) {
    return {.title = core::tr("gui.confirm_delete_title"),
            .message = message,
            .buttons = {{core::tr("gui.delete"), true}, {core::tr("gui.cancel"), false}},
            .default_button = 1,
            .escape_button = 1,
            .danger_button = 0};
}

}  // namespace noteapp::gui
