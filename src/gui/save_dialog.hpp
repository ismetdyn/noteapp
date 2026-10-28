#pragma once

#include <QDialog>
#include <QString>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QMessageBox;

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
    QString hint;  // "öneri: java %82"; taşıma kipinde boş
};

enum class ConflictChoice { Reload, Overwrite, SaveCopy, Cancel };
enum class CloseChoice { Save, Discard, Cancel };

// Ad / klasör / tip diyaloğu. Doğrulama hatası diyaloğu kapatmaz, satır içinde gösterilir.
// Q_OBJECT gerekmez: yalnız sanal accept() ezilir, düğmeler QDialog::accept'e bağlanır.
class SaveDialog : public QDialog {
public:
    SaveDialog(const SaveDialogRequest& request, QWidget* parent);

    SaveDetails details() const;
    void accept() override;

private:
    QLineEdit* name_ = nullptr;
    QComboBox* folder_ = nullptr;
    QComboBox* type_ = nullptr;
    QLabel* error_ = nullptr;
};

// Üretim varsayılanları: gerçek diyaloğu gösterir. nullopt = kullanıcı vazgeçti.
std::optional<SaveDetails> run_save_dialog(const SaveDialogRequest& request, QWidget* parent);
// Varsayılan düğme "benimkini kopya olarak kaydet", Esc = vazgeç (test edilebilsin diye ayrı üretilir).
std::unique_ptr<QMessageBox> make_conflict_box(QWidget* parent, const std::string& path);
ConflictChoice ask_conflict(QWidget* parent, const std::string& path);
CloseChoice ask_close_unsaved(QWidget* parent);

}  // namespace noteapp::gui
