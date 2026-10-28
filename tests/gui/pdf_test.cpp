// T33: başsız `notes-gui --export-pdf` (gerçek ikili, alt süreç) ve Ctrl+E akışı.
#include "gui/pdf_export.hpp"
#include "support/run_process.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTemporaryDir>
#include <doctest/doctest.h>

using gui_test::WindowFixture;
using noteapp::test::run_process;
namespace fs = std::filesystem;

namespace {

// Alt süreç gerçek kullanıcının config'ine dokunmasın ve pencere sistemi istemesin.
struct HeadlessRun {
    noteapp::test::ScopedEnv platform{"QT_QPA_PLATFORM", std::string("offscreen")};
    QTemporaryDir home;
    noteapp::test::ScopedEnv home_env{"NOTEAPP_HOME"};
    HeadlessRun() {
        REQUIRE(home.isValid());
        home_env.set(home.path().toStdString());
    }
    static noteapp::test::RunResult run(const std::vector<std::string>& args) {
        return run_process(fs::path(NOTEAPP_NOTES_GUI_EXE), args);
    }
    static noteapp::test::RunResult run(const QString& input, const QString& output) {
        return run({"--export-pdf", input.toStdString(), output.toStdString()});
    }
};

QByteArray slurp_bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void write_bytes(const QString& path, const QByteArray& content) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

void check_headless_export(const QString& name, const QByteArray& content) {
    HeadlessRun env;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString input = dir.filePath(name);
    const QString output = dir.filePath(QStringLiteral("çıktı.pdf"));
    write_bytes(input, content);

    const auto result = HeadlessRun::run(input, output);

    CHECK_MESSAGE(result.exit_code == 0, result.err);
    const QByteArray pdf = slurp_bytes(output);
    CHECK(pdf.startsWith("%PDF-"));
    CHECK(pdf.size() > 1000);
}

}  // namespace

TEST_CASE("notes-gui --export-pdf exports a Turkish-named markdown note") {
    check_headless_export(QStringLiteral("toplantı-notları.md"), "# Başlık\n\n**kalın** `kod` çğıİöşü\n");
}

TEST_CASE("notes-gui --export-pdf exports a Turkish-named plain text note") {
    check_headless_export(QStringLiteral("yapılacaklar.txt"), "bütçe onayı ÇĞIÖŞÜ\n");
}

TEST_CASE("notes-gui --export-pdf fails with exit 1 and a stderr message for a missing input") {
    HeadlessRun env;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("çıktı.pdf"));

    const auto result = HeadlessRun::run(dir.filePath(QStringLiteral("yok.md")), output);

    CHECK(result.exit_code == 1);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(QFile::exists(output));
}

TEST_CASE("notes-gui --export-pdf fails with exit 1 for an unwritable output and leaves no partial file") {
    HeadlessRun env;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString input = dir.filePath(QStringLiteral("a.md"));
    write_bytes(input, "# a\n");
    const QString output = dir.filePath(QStringLiteral("yok-dizin/çıktı.pdf"));  // üst dizin yok

    const auto result = HeadlessRun::run(input, output);

    CHECK(result.exit_code == 1);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(QFile::exists(output));
    CHECK(QDir(dir.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size() == 1);  // yalnız a.md
}

TEST_CASE("notes-gui --export-pdf with a wrong argument count exits 1 with the usage text") {
    HeadlessRun env;
    const auto result = HeadlessRun::run({"--export-pdf"});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find("--export-pdf") != std::string::npos);
}

namespace {

using noteapp::gui::MainWindow;

struct ExportFixture : WindowFixture {
    QTemporaryDir out;
    int asked = 0;
    QString suggested;

    void wire(MainWindow& window, std::optional<QString> answer) {
        window.deciders.export_path = [this, answer](const QString& suggestion) {
            ++asked;
            suggested = suggestion;
            return answer;
        };
    }
    QString out_path() const { return out.filePath(QStringLiteral("çıktı.pdf")); }
};

}  // namespace

TEST_CASE_FIXTURE(ExportFixture, "export flushes a dirty buffer first, then writes the pdf and reports it") {
    add_note("alpha.md", "old");
    auto window = make_window();
    auto* editor = window->findChild<QPlainTextEdit*>("noteEditor");
    REQUIRE(editor);
    editor->setPlainText("# yeni içerik çğış");
    wire(*window, out_path());

    window->export_pdf();

    CHECK(read_note("alpha.md") == "# yeni içerik çğış");  // PDF'in kaynağı diskteki sürüm
    CHECK(slurp_bytes(out_path()).startsWith("%PDF-"));
    CHECK(suggested.endsWith("alpha.pdf"));
    CHECK(window->statusBar()->currentMessage().contains(out_path()));
}

TEST_CASE_FIXTURE(ExportFixture, "cancelled export path writes no pdf") {
    add_note("alpha.md", "# a");
    auto window = make_window();
    wire(*window, std::nullopt);

    window->export_pdf();

    CHECK(asked == 1);
    CHECK(QDir(out.path()).isEmpty());
}

TEST_CASE_FIXTURE(ExportFixture, "export failure is shown in the status bar") {
    add_note("alpha.md", "# a");
    auto window = make_window();
    const QString bad = out.filePath(QStringLiteral("yok-dizin/x.pdf"));
    wire(*window, bad);

    window->export_pdf();

    CHECK_FALSE(QFile::exists(bad));
    CHECK(window->statusBar()->currentMessage().contains(QStringLiteral("Operation failed")));
}

TEST_CASE_FIXTURE(ExportFixture, "export does nothing for a binary note") {
    add_note("blob.bin", QByteArray("ab\0cd", 5));
    auto window = make_window();
    wire(*window, out_path());

    window->export_pdf();

    CHECK(asked == 0);
    CHECK(QDir(out.path()).isEmpty());
}

TEST_CASE_FIXTURE(ExportFixture, "export does nothing when no note is shown") {
    auto window = make_window();
    wire(*window, out_path());

    window->export_pdf();

    CHECK(asked == 0);
}
