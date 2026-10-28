#include "gui/save_dialog.hpp"

#include "core/i18n.hpp"
#include "core/note_path.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace noteapp::gui {

namespace {

QString text(const char* key, const core::Args& args = {}) {
    return QString::fromStdString(core::tr(key, args));
}

QString qs(const std::string& utf8) {
    return QString::fromStdString(utf8);
}

QComboBox* make_combo(const std::vector<std::string>& items, const std::string& current, bool root_first) {
    auto* box = new QComboBox;
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    if (root_first)
        box->addItem(QString{});  // kök klasör: boş metin
    for (const std::string& item : items)
        box->addItem(qs(item));
    box->setEditText(qs(current));
    return box;
}

// NotePath kurallarının tamamı tek yerde (ad + klasör + tip); ilk ihlalin metni, yoksa boş.
QString validate(const SaveDetails& d) {
    if (d.name.empty())
        return text("gui.err_name_empty");
    if (d.name.find_first_of("/\\") != std::string::npos)
        return text("gui.err_name_separator");
    if (d.type.empty() || d.type.find('.') != std::string::npos)
        return text("gui.err_type_invalid");
    if (!d.folder.empty()) {
        if (const auto folder = core::NotePath::parse(d.folder); !folder)
            return qs(core::format_error(folder.error()));
    }
    const auto path = core::NotePath::parse((d.folder.empty() ? "" : d.folder + "/") + d.name + "." + d.type);
    return path ? QString{} : qs(core::format_error(path.error()));
}

}  // namespace

SaveDialog::SaveDialog(const SaveDialogRequest& request, QWidget* parent) : QDialog(parent) {
    const bool move = request.mode == SaveDialogRequest::Mode::Move;
    setWindowTitle(text(move ? "gui.move_dialog_title" : "gui.save_dialog_title"));

    name_ = new QLineEdit(qs(request.initial.name));
    name_->setObjectName("nameEdit");
    name_->setAccessibleName(text("gui.field_name"));
    folder_ = make_combo(request.folders, request.initial.folder, true);
    folder_->setObjectName("folderBox");
    folder_->setAccessibleName(text("gui.field_folder"));
    folder_->lineEdit()->setPlaceholderText(text("gui.folder_root"));
    type_ = make_combo(request.types, request.initial.type, false);
    type_->setObjectName("typeBox");
    type_->setAccessibleName(text("gui.field_type"));

    auto* form = new QFormLayout;
    form->addRow(text("gui.field_name"), name_);
    form->addRow(text("gui.field_folder"), folder_);
    form->addRow(text("gui.field_type"), type_);

    auto* hint = new QLabel(request.hint);
    hint->setObjectName("suggestionLabel");
    hint->setVisible(!request.hint.isEmpty());
    error_ = new QLabel;
    error_->setObjectName("errorLabel");
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    error_->setAccessibleName(text("gui.a11y_error"));

    auto* buttons = new QDialogButtonBox;
    buttons->addButton(text("gui.ok"), QDialogButtonBox::AcceptRole)->setDefault(true);
    buttons->addButton(text("gui.cancel"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(error_);
    layout->addWidget(buttons);
    setMinimumWidth(420);
    if (request.folder_only) {
        name_->setEnabled(false);
        type_->setEnabled(false);
        folder_->setFocus();
    } else {
        name_->setFocus();
        name_->selectAll();
    }
}

SaveDetails SaveDialog::details() const {
    std::string type = type_->currentText().trimmed().toStdString();
    if (!type.empty() && type.front() == '.')
        type.erase(0, 1);  // ".txt" elle yazılabilir
    return {name_->text().trimmed().toStdString(), folder_->currentText().trimmed().toStdString(), type};
}

void SaveDialog::accept() {
    const QString problem = validate(details());
    error_->setText(problem);
    if (problem.isEmpty())
        QDialog::accept();
}

std::optional<SaveDetails> run_save_dialog(const SaveDialogRequest& request, QWidget* parent) {
    SaveDialog dialog(request, parent);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return dialog.details();
}

std::unique_ptr<QMessageBox> make_conflict_box(QWidget* parent, const std::string& path) {
    auto box = std::make_unique<QMessageBox>(QMessageBox::Warning, text("gui.conflict_title"),
                                             text("gui.conflict_text", {{"path", path}}),
                                             QMessageBox::NoButton, parent);
    box->addButton(text("gui.conflict_reload"), QMessageBox::DestructiveRole)->setObjectName("reload");
    box->addButton(text("gui.conflict_overwrite"), QMessageBox::DestructiveRole)->setObjectName("overwrite");
    QPushButton* copy = box->addButton(text("gui.conflict_copy"), QMessageBox::AcceptRole);
    copy->setObjectName("copy");
    QPushButton* cancel = box->addButton(text("gui.cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(copy);  // veri kaybettirmeyen seçenek varsayılan
    box->setEscapeButton(cancel);
    return box;
}

ConflictChoice ask_conflict(QWidget* parent, const std::string& path) {
    const auto box = make_conflict_box(parent, path);
    box->exec();
    const QString chosen = box->clickedButton() ? box->clickedButton()->objectName() : QString{};
    if (chosen == "reload")
        return ConflictChoice::Reload;
    if (chosen == "overwrite")
        return ConflictChoice::Overwrite;
    if (chosen == "copy")
        return ConflictChoice::SaveCopy;
    return ConflictChoice::Cancel;
}

CloseChoice ask_close_unsaved(QWidget* parent) {
    QMessageBox box(QMessageBox::Question, text("gui.close_title"), text("gui.close_text"),
                    QMessageBox::NoButton, parent);
    QPushButton* save = box.addButton(text("gui.close_save"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(text("gui.close_discard"), QMessageBox::DestructiveRole);
    QPushButton* cancel = box.addButton(text("gui.cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(save);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() == save)
        return CloseChoice::Save;
    return box.clickedButton() == discard ? CloseChoice::Discard : CloseChoice::Cancel;
}

std::unique_ptr<QMessageBox> make_delete_box(QWidget* parent, const QString& message) {
    auto box = std::make_unique<QMessageBox>(QMessageBox::Warning, text("gui.confirm_delete_title"), message,
                                             QMessageBox::NoButton, parent);
    box->addButton(text("gui.delete"), QMessageBox::DestructiveRole)->setObjectName("delete");
    QPushButton* cancel = box->addButton(text("gui.cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(cancel);  // geri alınamaz işlem: Enter silmesin
    box->setEscapeButton(cancel);
    return box;
}

bool ask_confirm_delete(QWidget* parent, const QString& message) {
    const auto box = make_delete_box(parent, message);
    box->exec();
    return box->clickedButton() && box->clickedButton()->objectName() == "delete";
}

std::optional<std::string> ask_folder_name(QWidget* parent, const QString& title, const QString& initial) {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(parent, title, text("gui.folder_name"), QLineEdit::Normal, initial, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty())
        return std::nullopt;
    return name.toStdString();
}

}  // namespace noteapp::gui
