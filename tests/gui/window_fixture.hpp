#pragma once

#include "core/config.hpp"
#include "core/i18n.hpp"
#include "gui/main_window.hpp"

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>

namespace gui_test {

using noteapp::gui::MainWindow;

// Geçici NOTEAPP_HOME + notes_dir; pencere testleri gerçek kullanıcı dizinine dokunmaz.
struct WindowFixture {
    int argc = 1;
    char arg0[10] = "gui_tests";
    char* argv[2] = {arg0, nullptr};
    QApplication app{argc, argv};
    QTemporaryDir home;
    noteapp::core::Config config;

    WindowFixture() {
        REQUIRE(home.isValid());
        qputenv("NOTEAPP_HOME", home.path().toUtf8());
        noteapp::core::init_i18n("en");
        config = noteapp::core::load_config().config;
        config.notes_dir = std::filesystem::path(home.filePath("notes").toStdU16String());
        std::filesystem::create_directories(config.notes_dir);
    }

    // age_minutes büyük = eski; sıralama mtime'a bağlı olduğundan açıkça ayarlanır.
    void add_note(const QString& relative, const QByteArray& content, int age_minutes = 0) {
        const QString full = QString::fromStdU16String(config.notes_dir.u16string()) + "/" + relative;
        std::filesystem::create_directories(std::filesystem::path(full.toStdU16String()).parent_path());
        QFile file(full);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(content);
        file.close();
        std::filesystem::last_write_time(full.toStdU16String(),
                                         std::filesystem::file_time_type::clock::now() -
                                             std::chrono::minutes(age_minutes));
    }

    QString full_path(const QString& relative) const {
        return QString::fromStdU16String(config.notes_dir.u16string()) + "/" + relative;
    }
    bool note_exists(const QString& relative) const { return QFile::exists(full_path(relative)); }
    QByteArray read_note(const QString& relative) const {
        QFile file(full_path(relative));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
    }

    std::unique_ptr<MainWindow> make_window() { return std::make_unique<MainWindow>(config); }
};

inline void press(QWidget* target, int key, const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(target, &event);
}

}  // namespace gui_test
