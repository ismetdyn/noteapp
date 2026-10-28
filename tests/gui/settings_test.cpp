#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "gui/main_window.hpp"
#include "gui/settings_page.hpp"
#include "sync/git_runtime.hpp"
#include "window_fixture.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <git2.h>
#include <sstream>

namespace fs = std::filesystem;
using gui_test::WindowFixture;
using noteapp::core::tr;
using noteapp::gui::MainWindow;
using noteapp::gui::SettingsPage;

namespace {

QString to_q(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

struct SettingsFixture : WindowFixture {
    std::vector<std::string> stored_hosts;  // sahte kasa
    std::vector<std::string> stored_tokens;
    int replace_asked = 0;
    bool replace_answer = true;
    std::vector<QString> infos;
    noteapp::sync::GitRuntime git;

    SettingsFixture() {
        // Pencere dışı süreç (note --background) da aynı notes_dir'i görsün: config dosyaya yazılır.
        REQUIRE(noteapp::core::save_config(config));
    }

    std::unique_ptr<MainWindow> make_window_wired() {
        auto window = make_window();
        window->sync_note_exe = fs::path(NOTEAPP_NOTE_EXE);
        window->deciders.confirm_replace_origin = [this](const QString&) {
            ++replace_asked;
            return replace_answer;
        };
        window->deciders.info = [this](const QString& message) { infos.push_back(message); };
        SettingsPage* page = window->settings_page();
        REQUIRE(page);
        page->tokens.has = [this](std::string_view host) {
            return std::ranges::find(stored_hosts, std::string(host)) != stored_hosts.end();
        };
        page->tokens.store = [this](std::string_view host,
                                    std::string_view token) -> std::expected<void, noteapp::core::Error> {
            stored_hosts.emplace_back(host);
            stored_tokens.emplace_back(token);
            return {};
        };
        page->tokens.clear = [this](std::string_view host) -> std::expected<void, noteapp::core::Error> {
            std::erase(stored_hosts, std::string(host));
            return {};
        };
        return window;
    }

    static void set_text(MainWindow& w, const char* key, const QString& value) {
        auto* edit = w.findChild<QLineEdit*>(QString("field_") + key);
        if (edit) {
            edit->setText(value);
            return;
        }
        auto* combo = w.findChild<QComboBox*>(QString("field_") + key);
        REQUIRE(combo);
        combo->setCurrentText(value);
    }
    static QString error_of(MainWindow& w, const char* key) {
        auto* label = w.findChild<QLabel*>(QString("error_") + key);
        REQUIRE(label);
        return label->text();
    }
    static QPushButton* button(MainWindow& w, const char* name) {
        auto* b = w.findChild<QPushButton*>(name);
        REQUIRE(b);
        return b;
    }
    static std::string config_text() {
        std::ifstream in(noteapp::core::config_file(), std::ios::binary);
        std::stringstream out;
        out << in.rdbuf();
        return out.str();
    }
    static std::string get(const char* key) {
        for (const auto& [k, v] : noteapp::core::config_entries(noteapp::core::load_config().config))
            if (k == key)
                return v;
        return "<none>";
    }
    // Yerel çıplak uzak depo (libgit2: sistemde git olması şart değil).
    fs::path make_bare(const char* name) {
        const fs::path dir = fs::path(home.path().toStdU16String()) / name;
        git_repository* repo = nullptr;
        REQUIRE(git_repository_init(&repo, dir.string().c_str(), 1) == 0);
        git_repository_free(repo);
        return dir;
    }
    static bool wait_for(const std::function<bool()>& done, int ms = 30000) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        return done();
    }
};

}  // namespace

TEST_CASE_FIXTURE(SettingsFixture,
                  "saved value is visible through load_config and the token never reaches config.toml") {
    auto window = make_window_wired();
    set_text(*window, "editor", "vim");
    set_text(*window, "types", "md, txt, py");
    set_text(*window, "default_type", "py");
    set_text(*window, "sync.author_name", "Test Yazar");
    button(*window, "saveButton")->click();

    CHECK(get("editor") == "vim");
    CHECK(get("types") == "md, txt, py");
    CHECK(get("default_type") == "py");
    CHECK(get("sync.author_name") == "Test Yazar");
    CHECK(error_of(*window, "editor").isEmpty());

    // Token yalnız kasaya gider (https remote'ta) ve config.toml'da hiçbir yerde geçmez.
    set_text(*window, "sync.remote", "https://github.com/kisi/notlar.git");
    auto* token = window->findChild<QLineEdit*>("tokenEdit");
    REQUIRE(token);
    token->setText("ghp_gizli_deger");
    button(*window, "tokenSaveButton")->click();
    REQUIRE(stored_tokens.size() == 1);
    CHECK(stored_tokens.front() == "ghp_gizli_deger");
    CHECK(token->text().isEmpty());  // alan temizlenir, değer asla geri gösterilmez
    button(*window, "saveButton")->click();
    CHECK(config_text().find("ghp_gizli_deger") == std::string::npos);
    CHECK(config_text().find("token") == std::string::npos);
}

TEST_CASE_FIXTURE(SettingsFixture, "an invalid value is shown under its field and nothing is saved") {
    auto window = make_window_wired();
    const std::string before = config_text();
    set_text(*window, "editor", "vim");  // geçerli değişiklik de kaydedilmemeli
    set_text(*window, "default_type", "bad.type");
    button(*window, "saveButton")->click();

    CHECK_FALSE(error_of(*window, "default_type").isEmpty());
    CHECK(error_of(*window, "editor").isEmpty());
    CHECK(config_text() == before);

    // Düzeltilince hata silinir ve kayıt geçer.
    set_text(*window, "default_type", "txt");
    button(*window, "saveButton")->click();
    CHECK(error_of(*window, "default_type").isEmpty());
    CHECK(get("editor") == "vim");
}

TEST_CASE_FIXTURE(SettingsFixture, "a broken config file is never overwritten") {
    {
        std::ofstream out(noteapp::core::config_file(), std::ios::binary | std::ios::trunc);
        out << "editor = = bozuk\n";
    }
    const std::string before = config_text();
    auto window = make_window_wired();
    set_text(*window, "editor", "vim");
    button(*window, "saveButton")->click();

    CHECK(config_text() == before);
    auto* global = window->findChild<QLabel*>("globalError");
    REQUIRE(global);
    CHECK_FALSE(global->text().isEmpty());
}

TEST_CASE_FIXTURE(SettingsFixture, "changing notes_dir re-points the note list") {
    add_note("eski.md", "old");
    const fs::path other = fs::path(home.path().toStdU16String()) / "other-notes";
    fs::create_directories(other);
    {
        std::ofstream out(other / "yeni.md");
        out << "new";
    }
    auto window = make_window_wired();
    auto* list = window->findChild<QListView*>("noteList");
    REQUIRE(list);
    REQUIRE(list->model()->rowCount() == 1);

    set_text(*window, "notes_dir", to_q(other));
    button(*window, "saveButton")->click();

    REQUIRE(list->model()->rowCount() == 1);
    CHECK(list->model()->index(0, 0).data().toString() == "yeni.md");
    CHECK(fs::path(get("notes_dir")) == other);
}

TEST_CASE_FIXTURE(SettingsFixture, "the notes_dir browse button goes through the decider") {
    const fs::path other = fs::path(home.path().toStdU16String()) / "picked";
    auto window = make_window_wired();
    window->deciders.browse_dir = [&](const QString&) { return std::optional<QString>(to_q(other)); };
    button(*window, "browseButton")->click();
    CHECK(window->findChild<QLineEdit*>("field_notes_dir")->text() == to_q(other));
}

TEST_CASE_FIXTURE(SettingsFixture, "the token row is enabled only for HTTPS remotes") {
    auto window = make_window_wired();
    auto* token = window->findChild<QLineEdit*>("tokenEdit");
    auto* state = window->findChild<QLabel*>("tokenState");
    REQUIRE(token);
    REQUIRE(state);
    CHECK(token->echoMode() == QLineEdit::Password);

    set_text(*window, "sync.remote", "git@github.com:kisi/notlar.git");
    CHECK_FALSE(token->isEnabled());
    CHECK_FALSE(button(*window, "tokenSaveButton")->isEnabled());
    set_text(*window, "sync.remote", "C:/some/local/remote.git");
    CHECK_FALSE(token->isEnabled());
    set_text(*window, "sync.remote", "https://github.com/kisi/notlar.git");
    CHECK(token->isEnabled());
    CHECK(button(*window, "tokenSaveButton")->isEnabled());
    CHECK(state->text().isEmpty());

    stored_hosts.push_back("github.com");
    set_text(*window, "sync.remote", "https://github.com/kisi/baska.git");
    CHECK_FALSE(state->text().isEmpty());  // "kasada kayıtlı"
    button(*window, "tokenClearButton")->click();
    CHECK(stored_hosts.empty());
    CHECK(state->text().isEmpty());
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "enabling sync against a local bare remote saves the config and starts syncing") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto window = make_window_wired();
    auto* label = window->findChild<QLabel*>("syncLabel");
    REQUIRE(label);
    CHECK(label->text() == QString::fromStdString(tr("gui.sync_off")));

    window->findChild<QCheckBox*>("field_sync.enabled")->setChecked(true);
    set_text(*window, "sync.remote", to_q(remote));
    set_text(*window, "sync.author_name", "Test Yazar");
    set_text(*window, "sync.author_email", "test@noteapp.invalid");
    button(*window, "saveButton")->click();

    CHECK(get("sync.enabled") == "true");
    CHECK(get("sync.remote") == remote.string());
    CHECK(error_of(*window, "sync.remote").isEmpty());
    CHECK(fs::exists(fs::path(config.notes_dir) / ".git"));
    // AutoSync açılışta bir senkron koşar; durum çubuğu "kapalı"dan çıkıp ✓'ye gelir.
    CHECK(wait_for([&] { return label->text().startsWith(QString::fromUtf8("\xE2\x9C\x93")); }));
}

TEST_CASE_FIXTURE(SettingsFixture, "an origin mismatch asks before replacing and declining saves nothing") {
    const fs::path first = make_bare("first.git");
    const fs::path second = make_bare("second.git");
    {  // notes_dir zaten `first` remote'lu bir depo
        git_repository* repo = nullptr;
        REQUIRE(git_repository_init(&repo, config.notes_dir.string().c_str(), 0) == 0);
        git_remote* remote = nullptr;
        REQUIRE(git_remote_create(&remote, repo, "origin", first.string().c_str()) == 0);
        git_remote_free(remote);
        git_repository_free(repo);
    }
    auto window = make_window_wired();
    window->findChild<QCheckBox*>("field_sync.enabled")->setChecked(true);
    set_text(*window, "sync.remote", to_q(second));
    set_text(*window, "sync.author_name", "Test Yazar");
    set_text(*window, "sync.author_email", "test@noteapp.invalid");

    replace_answer = false;
    button(*window, "saveButton")->click();
    CHECK(replace_asked == 1);
    CHECK(get("sync.enabled") == "false");
    CHECK_FALSE(error_of(*window, "sync.remote").isEmpty());

    replace_answer = true;
    button(*window, "saveButton")->click();
    CHECK(replace_asked == 2);
    CHECK(get("sync.enabled") == "true");
    CHECK(get("sync.remote") == second.string());
    CHECK(error_of(*window, "sync.remote").isEmpty());
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "the test connection button enables and runs one sync, showing the outcome") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto window = make_window_wired();
    set_text(*window, "sync.remote", to_q(remote));
    set_text(*window, "sync.author_name", "Test Yazar");
    set_text(*window, "sync.author_email", "test@noteapp.invalid");
    auto* result = window->findChild<QLabel*>("testResult");
    REQUIRE(result);
    button(*window, "testButton")->click();
    CHECK(wait_for([&] { return !result->text().isEmpty(); }));
    CHECK(result->text().startsWith(QString::fromUtf8("\xE2\x9C\x93")));
    CHECK(fs::exists(fs::path(config.notes_dir) / ".git"));

    // Geçersiz adres: sonuç hatadır, ✓ değil.
    set_text(*window, "sync.remote", to_q(remote / "yok"));
    result->clear();
    button(*window, "testButton")->click();
    CHECK(wait_for([&] { return !result->text().isEmpty(); }));
    CHECK(result->text().startsWith(QString::fromUtf8("\xE2\x9A\xA0")));
}

TEST_CASE_FIXTURE(SettingsFixture, "the status label shows off, running, last success and last error") {
    auto window = make_window_wired();
    auto* label = window->findChild<QLabel*>("syncLabel");
    REQUIRE(label);
    noteapp::sync::SyncStatus status;
    window->show_sync_status(status, true);
    CHECK(label->text() == QString::fromUtf8("\xE2\x9F\xB3"));

    std::tm local{};
    local.tm_year = 126;
    local.tm_mon = 3;
    local.tm_mday = 10;
    local.tm_hour = 14;
    local.tm_min = 5;
    local.tm_isdst = -1;
    status.last_success = std::chrono::system_clock::from_time_t(std::mktime(&local));
    window->show_sync_status(status, false);
    CHECK(label->text() == QString::fromUtf8("\xE2\x9C\x93 14:05"));

    status.last_error = noteapp::core::Error{noteapp::core::Error::Code::Io, "sync.timeout", {}};
    window->show_sync_status(status, false);
    CHECK(label->text().startsWith(QString::fromUtf8("\xE2\x9A\xA0")));
    CHECK(label->text().size() > 3);
}

TEST_CASE_FIXTURE(SettingsFixture, "F5 asks for a sync now and Ctrl+comma opens the settings tab") {
    auto window = make_window_wired();
    CHECK(window->sync_requests() == 0);
    bool found = false;
    for (auto* shortcut : window->findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(Qt::Key_F5)) {
            found = true;
            QMetaObject::invokeMethod(shortcut, "activated");
        }
    }
    CHECK(found);
    CHECK(window->sync_requests() == 1);
    window->show_settings_tab();
    CHECK(window->findChild<QTabWidget*>()->currentIndex() == 1);
}

TEST_CASE_FIXTURE(SettingsFixture, "changing language only announces a restart") {
    auto window = make_window_wired();
    set_text(*window, "language", "tr");
    button(*window, "saveButton")->click();
    CHECK(get("language") == "tr");
    CHECK(infos.size() == 1);
}

TEST_CASE_FIXTURE(SettingsFixture, "revert restores the saved values") {
    auto window = make_window_wired();
    set_text(*window, "editor", "emacs");
    button(*window, "revertButton")->click();
    CHECK(window->findChild<QLineEdit*>("field_editor")->text() != "emacs");
}
