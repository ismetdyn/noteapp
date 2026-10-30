#include "core/i18n.hpp"
#include "gui/pdf_export.hpp"

#include <QAbstractTextDocumentLayout>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>
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
        FAIL(noteapp::core::format_error(result.error()));
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

namespace {

// Belgenin kaynak yüklemesini sayar: görsel istenirse burası çağrılır.
struct CountingDocument : QTextDocument {
    int loads = 0;
    QVariant loadResource(int type, const QUrl& name) override {
        ++loads;
        return QTextDocument::loadResource(type, name);
    }
};

int image_fragments(const QTextDocument& document) {
    int count = 0;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            count += it.fragment().charFormat().isImageFormat() ? 1 : 0;
    return count;
}

// Gürültülü PNG: PDF'e gömülürse dosya belirgin büyür.
QString write_noise_png(const QString& path) {
    QImage image(300, 300, QImage::Format_RGB32);
    quint32 state = 12345;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            state = state * 1664525u + 1013904223u;
            image.setPixel(x, y, state | 0xff000000u);
        }
    REQUIRE(image.save(path, "PNG"));
    return path;
}

}  // namespace

TEST_CASE_FIXTURE(GuiAppFixture, "strip_images replaces every image fragment with its alt text") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString png = write_noise_png(dir.filePath(QStringLiteral("secret.png")));
    const QString markdown = QStringLiteral("önce ![gizli alt](%1) ve ![x](file://attacker.example/s/a.png) "
                                            "ve ![](file:///C:/Users/u/id.jpg) sonra")
                                 .arg(QUrl::fromLocalFile(png).toString());
    CountingDocument document;
    document.setMarkdown(markdown);
    REQUIRE(image_fragments(document) == 2);  // ön koşul (Qt boş alt metinli görseli zaten düşürür)

    noteapp::gui::strip_images(document);

    CHECK(image_fragments(document) == 0);
    const QString plain = document.toPlainText();
    CHECK(plain.contains(QStringLiteral("gizli alt")));
    CHECK(plain.contains(QStringLiteral("önce")));
    CHECK(plain.contains(QStringLiteral("sonra")));
    document.documentLayout()->documentSize();  // düzen, görsel isteyebilecek en geç andır
    CHECK(document.loads == 0);
}

TEST_CASE_FIXTURE(GuiAppFixture, "export_pdf never embeds or requests images referenced by a note") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString png = write_noise_png(dir.filePath(QStringLiteral("secret.png")));
    const QString input = dir.filePath(QStringLiteral("n.md"));
    const QString output = dir.filePath(QStringLiteral("n.pdf"));
    write_file(input, QStringLiteral("# Not\n\n![gizli alt](%1)\n\n![](file://attacker.example/s/a.png)\n\n"
                                     "<img src=\"file://attacker.example/s/b.png\">\n")
                          .arg(QUrl::fromLocalFile(png).toString())
                          .toUtf8());

    const auto result = noteapp::gui::export_pdf(input, output);

    REQUIRE(result.has_value());
    // Gömülseydi PDF en az PNG kadar büyürdü.
    CHECK(read_file(output).size() < QFileInfo(png).size() / 2);
}
