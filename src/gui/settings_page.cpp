#include "gui/settings_page.hpp"

#include "core/i18n.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <thread>

namespace noteapp::gui {

namespace {

const char kErrorStyle[] = "color: #c0392b;";  // açık ve koyu temada okunur kırmızı

QString text(const char* key, const core::Args& args = {}) {
    return QString::fromStdString(core::tr(key, args));
}

// "sync.remote" -> "gui.setting_sync_remote": etiket metinleri katalogdan gelir.
QString label_for(const std::string& key) {
    std::string catalog_key = "gui.setting_" + key;
    std::ranges::replace(catalog_key, '.', '_');
    return QString::fromStdString(core::tr(catalog_key));
}

// Token yalnız https remote'a aittir; kasa hesabı host'tur. Https değilse boş.
std::string https_host(const std::string& remote) {
    const sync::RemoteEndpoint endpoint = sync::parse_remote(remote);
    return endpoint.auth == sync::Auth::Https ? endpoint.host : std::string{};
}

}  // namespace

struct SettingsPage::Field {
    QWidget* input = nullptr;
    QLabel* error = nullptr;
};

SettingsPage::SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host, QWidget* parent)
    : QWidget(parent), applied_(std::move(config)), deciders_(deciders), host_(std::move(host)) {
    tokens.has = [](std::string_view h) { return sync::has_token(h); };
    tokens.store = [](std::string_view h, std::string_view t) { return sync::store_token(h, t); };
    tokens.clear = [](std::string_view h) { return sync::clear_token(h); };
    build_ui();
    load(applied_);
}

SettingsPage::~SettingsPage() {
    if (worker_.joinable())
        worker_.join();
}

void SettingsPage::build_ui() {
    auto* form = new QFormLayout;
    for (const auto& [key, ignored] : core::config_entries(applied_)) {
        auto* field = new Field;
        QWidget* input = nullptr;
        if (key == "language") {
            auto* box = new QComboBox;
            box->addItems({"auto", "tr", "en"});
            input = box;
        } else if (key == "default_type") {
            auto* box = new QComboBox;
            box->setEditable(true);
            input = box;
        } else if (key == "sync.enabled") {
            input = new QCheckBox;
        } else if (key == "sync.idle_seconds") {
            auto* spin = new QSpinBox;
            spin->setRange(1, INT_MAX);
            input = spin;
        } else {
            input = new QLineEdit;
        }
        input->setObjectName(QString("field_") + QString::fromStdString(key));
        input->setAccessibleName(label_for(key));
        field->input = input;
        field->error = new QLabel;
        field->error->setObjectName(QString("error_") + QString::fromStdString(key));
        field->error->setAccessibleName(text("gui.a11y_error"));
        field->error->setStyleSheet(kErrorStyle);
        field->error->setWordWrap(true);
        fields_[key] = field;

        auto* cell = new QWidget;
        auto* column = new QVBoxLayout(cell);
        column->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout;
        row->addWidget(input, 1);
        if (key == "notes_dir") {
            auto* browse = new QPushButton(text("gui.browse"));
            browse->setObjectName("browseButton");
            connect(browse, &QPushButton::clicked, this, [this] {
                auto* edit = static_cast<QLineEdit*>(fields_.at("notes_dir")->input);
                if (const auto chosen = deciders_.browse_dir(edit->text()))
                    edit->setText(*chosen);
            });
            row->addWidget(browse);
        }
        column->addLayout(row);
        column->addWidget(field->error);
        form->addRow(label_for(key), cell);
    }

    // Token: yalnız https remote'ta etkin; değer asla gösterilmez, config'e yazılmaz.
    token_edit_ = new QLineEdit;
    token_edit_->setObjectName("tokenEdit");
    token_edit_->setEchoMode(QLineEdit::Password);
    token_edit_->setAccessibleName(text("gui.token_label"));
    token_save_ = new QPushButton(text("gui.token_save"));
    token_save_->setObjectName("tokenSaveButton");
    token_clear_ = new QPushButton(text("gui.token_clear"));
    token_clear_->setObjectName("tokenClearButton");
    token_state_ = new QLabel;
    token_state_->setObjectName("tokenState");
    auto* token_row = new QHBoxLayout;
    token_row->addWidget(token_edit_, 1);
    token_row->addWidget(token_save_);
    token_row->addWidget(token_clear_);
    auto* token_cell = new QWidget;
    auto* token_column = new QVBoxLayout(token_cell);
    token_column->setContentsMargins(0, 0, 0, 0);
    token_column->addLayout(token_row);
    token_column->addWidget(token_state_);
    form->addRow(text("gui.token_label"), token_cell);

    test_button_ = new QPushButton(text("gui.test_connection"));
    test_button_->setObjectName("testButton");
    test_result_ = new QLabel;
    test_result_->setObjectName("testResult");
    test_result_->setWordWrap(true);
    form->addRow(test_button_, test_result_);

    global_error_ = new QLabel;
    global_error_->setObjectName("globalError");
    global_error_->setStyleSheet(kErrorStyle);
    global_error_->setWordWrap(true);

    auto* save_button = new QPushButton(text("gui.settings_save"));
    save_button->setObjectName("saveButton");
    auto* revert_button = new QPushButton(text("gui.settings_revert"));
    revert_button->setObjectName("revertButton");
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(revert_button);
    buttons->addWidget(save_button);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(global_error_);
    layout->addLayout(buttons);
    layout->addStretch(1);

    connect(save_button, &QPushButton::clicked, this, [this] { save(); });
    connect(revert_button, &QPushButton::clicked, this, [this] { revert(); });
    connect(token_save_, &QPushButton::clicked, this, [this] { save_token(); });
    connect(token_clear_, &QPushButton::clicked, this, [this] { clear_token(); });
    connect(test_button_, &QPushButton::clicked, this, [this] { test_connection(); });
    connect(static_cast<QLineEdit*>(fields_.at("sync.remote")->input), &QLineEdit::textChanged, this,
            [this] { refresh_token_row(); });
}

void SettingsPage::load(const core::Config& config) {
    for (const auto& [key, value] : core::config_entries(config)) {
        QWidget* input = fields_.at(key)->input;
        const QString shown = QString::fromStdString(value);
        if (auto* box = qobject_cast<QComboBox*>(input)) {
            if (key == "default_type") {  // seçenekler types'tan: yeni tip eklenince hemen önerilir
                box->clear();
                for (const std::string& type : config.types)
                    box->addItem(QString::fromStdString(type));
            }
            box->setCurrentText(shown);
        } else if (auto* check = qobject_cast<QCheckBox*>(input)) {
            check->setChecked(value == "true");
        } else if (auto* spin = qobject_cast<QSpinBox*>(input)) {
            spin->setValue(std::stoi(value));
        } else {
            static_cast<QLineEdit*>(input)->setText(shown);
        }
    }
    clear_errors();
    refresh_token_row();
}

std::string SettingsPage::value_of(const std::string& key) const {
    QWidget* input = fields_.at(key)->input;
    if (const auto* box = qobject_cast<QComboBox*>(input))
        return box->currentText().toStdString();
    if (const auto* check = qobject_cast<QCheckBox*>(input))
        return check->isChecked() ? "true" : "false";
    if (const auto* spin = qobject_cast<QSpinBox*>(input))
        return std::to_string(spin->value());
    return static_cast<QLineEdit*>(input)->text().toStdString();
}

void SettingsPage::show_error(const std::string& key, const QString& message) {
    fields_.at(key)->error->setText(message);
}

void SettingsPage::clear_errors() {
    for (const auto& [key, field] : fields_)
        field->error->clear();
    global_error_->clear();
}

void SettingsPage::refresh_token_row() {
    const std::string host = https_host(value_of("sync.remote"));
    const bool https = !host.empty();
    token_edit_->setEnabled(https);
    token_save_->setEnabled(https);
    token_clear_->setEnabled(https);
    token_state_->setText(https && tokens.has(host) ? text("gui.token_stored") : QString{});
}

void SettingsPage::revert() {
    load(applied_);
}

std::expected<core::Config, QString> SettingsPage::load_config_for_update() const {
    // CLI `config set` ile aynı kural: bozuk dosya varsayılanlarla ezilip kullanıcının ayarları silinmesin.
    core::LoadedConfig fresh = core::load_config();
    for (const core::Error& warning : fresh.warnings)
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable")
            return std::unexpected(QString::fromStdString(core::format_error(warning)) + QLatin1Char(' ') +
                                   text("gui.settings_not_saved"));
    return std::move(fresh.config);
}

bool SettingsPage::apply_form(core::Config& target) {
    bool valid = true;
    // Yalnız değişen anahtarlar uygulanır: dokunulmayan ayar (ör. elle yazılmış, formda görünmeyen biçim)
    // kaydedilirken yeniden yazılmaz. Doğrulama set_config_value'da; kural burada kopyalanmaz.
    for (const auto& [key, saved_value] : core::config_entries(applied_)) {
        const std::string now = value_of(key);
        if (now == saved_value)
            continue;
        if (const auto set = core::set_config_value(target, key, now); !set) {
            show_error(key, QString::fromStdString(core::format_error(set.error())));
            valid = false;
        }
    }
    return valid;
}

// ponytail: enable ağa çıkabilir ve UI iş parçacığında koşar (bekleme imleci); yavaş uzaklarda pencere
// donarsa işçi iş parçacığına alınır. Karar penceresi (uyuşmazlık) zaten UI iş parçacığı ister.
QString SettingsPage::enable_sync(const core::Config& candidate) {
    struct WaitCursor {
        WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
        ~WaitCursor() { QApplication::restoreOverrideCursor(); }
    } wait;
    sync::SyncService service(candidate);
    auto result = service.enable(candidate.sync.remote);
    if (!result && result.error().message_key == "sync.remote_mismatch") {
        const QString message = QString::fromStdString(core::format_error(result.error()));
        if (!deciders_.confirm_replace_origin(message))
            return message;  // reddedildi: hiçbir şey kaydedilmez, açıklama alanın altında kalır
        result = service.enable(candidate.sync.remote, true);
    }
    return result ? QString{} : QString::fromStdString(core::format_error(result.error()));
}

void SettingsPage::save() {
    clear_errors();
    const auto fresh = load_config_for_update();
    if (!fresh) {
        global_error_->setText(fresh.error());
        return;
    }
    core::Config live = applied_;  // canlı uygulanacak (tuning dahil, çalışan config)
    if (!apply_form(live))
        return;
    core::Config persisted = *fresh;  // dosyaya yazılacak: diskteki ayarların üstüne aynı değişiklikler
    if (!apply_form(persisted))
        return;

    const bool dir_changed = live.notes_dir != applied_.notes_dir;
    if (dir_changed && !host_.flush()) {
        global_error_->setText(text("gui.settings_flush_failed"));
        return;
    }
    if (live.sync.enabled && (live.sync != applied_.sync || dir_changed)) {
        if (const QString error = enable_sync(live); !error.isEmpty()) {
            show_error("sync.remote", error);
            return;
        }
    }
    if (const auto saved = core::save_config(persisted); !saved) {
        global_error_->setText(QString::fromStdString(core::format_error(saved.error())) + QLatin1Char(' ') +
                               text("gui.settings_not_saved"));
        return;
    }
    const core::Config before = std::move(applied_);
    applied_ = live;
    load(applied_);  // normalleşmiş değerler (ör. "md,py" -> "md, py") forma döner
    host_.applied(before, applied_);
}

void SettingsPage::save_token() {
    const std::string host = https_host(value_of("sync.remote"));
    if (host.empty())
        return;
    if (const auto stored = tokens.store(host, token_edit_->text().toStdString()); !stored) {
        global_error_->setText(QString::fromStdString(core::format_error(stored.error())));
        return;
    }
    global_error_->clear();
    token_edit_->clear();  // token ekranda bekletilmez
    refresh_token_row();
}

void SettingsPage::clear_token() {
    const std::string host = https_host(value_of("sync.remote"));
    if (host.empty())
        return;
    if (const auto cleared = tokens.clear(host); !cleared) {
        global_error_->setText(QString::fromStdString(core::format_error(cleared.error())));
        return;
    }
    global_error_->clear();
    refresh_token_row();
}

void SettingsPage::test_connection() {
    if (testing_)
        return;
    clear_errors();
    test_result_->clear();
    core::Config candidate = applied_;
    if (!apply_form(candidate))
        return;
    if (const QString error = enable_sync(candidate); !error.isEmpty()) {
        test_result_->setText(text("gui.test_failed", {{"detail", error.toStdString()}}));
        return;
    }
    testing_ = true;
    test_button_->setEnabled(false);
    if (worker_.joinable())
        worker_.join();  // önceki iş bitti (testing_ false iken yalnız birikmiş iş parçacığı)
    // Ağ senkronu UI'yi dondurmasın diye işçide; sonuç kuyruktan UI iş parçacığına döner.
    worker_ = std::thread([this, candidate = std::move(candidate)] {
        auto outcome = sync::SyncService(candidate).sync_now();
        QMetaObject::invokeMethod(
            this,
            [this, outcome = std::move(outcome)] {
                testing_ = false;
                test_button_->setEnabled(true);
                test_result_->setText(
                    outcome ? text("gui.test_ok")
                            : text("gui.test_failed", {{"detail", core::format_error(outcome.error())}}));
                if (outcome && host_.synced)
                    host_.synced();  // çekilen notlar listeye yansısın
            },
            Qt::QueuedConnection);
    });
}

}  // namespace noteapp::gui
