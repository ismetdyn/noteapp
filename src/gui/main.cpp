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
        std::fputs("kullanım: notes-gui --export-pdf <girdi> <çıktı>\n", stderr);  // TODO(T09): i18n
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
    QApplication app(argc, argv);
    // argv değil arguments(): Windows'ta argv ANSI kod sayfasındadır, Türkçe karakterli yollar bozulur.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && args.at(1) == QLatin1StringView("--export-pdf")) {
        return run_pdf_export(args);
    }

    // TODO(T09): i18n — sekme adları ve yer tutucu metinler.
    QMainWindow window;
    auto* tabs = new QTabWidget(&window);
    tabs->addTab(new QLabel(QStringLiteral("Notlar burada listelenecek — çğıİöşü ÇĞIÖŞÜ")),
                 QStringLiteral("Notlar"));
    tabs->addTab(new QLabel(QStringLiteral("Ayarlar burada olacak")), QStringLiteral("Ayarlar"));
    window.setCentralWidget(tabs);
    window.setWindowTitle(QStringLiteral("noteapp"));
    window.resize(900, 600);
    window.show();
    return QApplication::exec();
}
