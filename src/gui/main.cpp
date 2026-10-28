#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/pdf_export.hpp"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QStringList>
#include <QTabWidget>
#include <cstdio>

namespace {

// notes-gui --export-pdf <girdi> <çıktı>: pencere açmadan dışa aktarır; çıkış kodu 0/1, hata stderr'e.
int run_pdf_export(const QStringList& args) {
    if (args.size() != 4) {
        std::fprintf(stderr, "%s\n", noteapp::core::tr("gui.usage_export_pdf").c_str());
        return 1;
    }
    const auto result = noteapp::gui::export_pdf(args.at(2), args.at(3));
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().toUtf8().constData());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer.
    const noteapp::core::LoadedConfig loaded = noteapp::core::load_config();
    noteapp::core::init_i18n(loaded.config.language);
    noteapp::core::init_log(loaded.config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        noteapp::core::log_warn(noteapp::core::format_error(warning));

    QApplication app(argc, argv);
    // argv değil arguments(): Windows'ta argv ANSI kod sayfasındadır, Türkçe karakterli yollar bozulur.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && args.at(1) == QLatin1StringView("--export-pdf")) {
        return run_pdf_export(args);
    }

    const auto text = [](const char* key) { return QString::fromUtf8(noteapp::core::tr(key)); };
    QMainWindow window;
    auto* tabs = new QTabWidget(&window);
    tabs->addTab(new QLabel(text("gui.notes_placeholder")), text("gui.tab_notes"));
    tabs->addTab(new QLabel(text("gui.settings_placeholder")), text("gui.tab_settings"));
    window.setCentralWidget(tabs);
    window.setWindowTitle(QStringLiteral("noteapp"));
    window.resize(900, 600);
    window.show();
    return QApplication::exec();
}
