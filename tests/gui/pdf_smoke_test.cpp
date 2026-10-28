#include "gui/pdf_export.hpp"

#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <doctest/doctest.h>

namespace {

constexpr char kTurkishSample[] = "çğıİöşü ÇĞIÖŞÜ";

// QPdfWriter ve font veritabanı canlı bir QGuiApplication ister; main doctest'e ait olduğundan test başına
// kurulur.
struct GuiAppFixture {
    int argc = 1;
    char arg0[10] = "gui_tests";
    char* argv[2] = {arg0, nullptr};
    QGuiApplication app{argc, argv};
};

void write_file(const QString& path, const QByteArray& content) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(content) == content.size());
}

QByteArray read_file(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

void check_exports_pdf(const QString& input_name, const QByteArray& content) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString input = dir.filePath(input_name);
    const QString output = dir.filePath(QStringLiteral("çıktı.pdf"));
    write_file(input, content);

    const auto result = noteapp::gui::export_pdf(input, output);

    if (!result) {
        FAIL(result.error().toStdString());
    }
    CHECK(read_file(output).startsWith("%PDF-"));
}

}  // namespace

TEST_CASE_FIXTURE(GuiAppFixture, "export_pdf renders a markdown note with Turkish text") {
    check_exports_pdf(QStringLiteral("toplantı-notları.md"),
                      QByteArray("# Toplantı notları\n\n- ") + kTurkishSample);
}

TEST_CASE_FIXTURE(GuiAppFixture, "export_pdf renders a plain text note with Turkish text") {
    check_exports_pdf(QStringLiteral("yapılacaklar.txt"), QByteArray("bütçe onayı\n") + kTurkishSample);
}

TEST_CASE_FIXTURE(GuiAppFixture, "export_pdf reports a missing input and writes nothing") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("çıktı.pdf"));

    const auto result = noteapp::gui::export_pdf(dir.filePath(QStringLiteral("yok.md")), output);

    CHECK_FALSE(result.has_value());
    CHECK_FALSE(QFile::exists(output));
}
