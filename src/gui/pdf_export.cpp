#include "gui/pdf_export.hpp"

#include "core/i18n.hpp"

#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <ranges>
#include <vector>

namespace noteapp::gui {

namespace {

constexpr qreal kPageMarginMm = 15.0;

// Katalog metni {path} ve {detail} ile doldurulur (core::format_error).
core::Error io_error(const char* key, const QString& path, const QString& detail) {
    return {core::Error::Code::Io, key, {{"path", path.toStdString()}, {"detail", detail.toStdString()}}};
}

}  // namespace

void strip_images(QTextDocument& document) {
    struct Image {
        int position;
        int length;
        QString replacement;
    };
    std::vector<Image> images;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (!format.isImageFormat())
                continue;
            QString alt = format.stringProperty(QTextFormat::ImageAltText);
            if (alt.isEmpty())
                alt = QString::fromStdString(core::tr("gui.pdf_image_placeholder"));
            images.push_back({fragment.position(), fragment.length(), std::move(alt)});
        }
    }
    // Sondan başa: önceki konumlar bozulmaz.
    for (const Image& image : std::views::reverse(images)) {
        QTextCursor cursor(&document);
        cursor.setPosition(image.position);
        cursor.setPosition(image.position + image.length, QTextCursor::KeepAnchor);
        cursor.insertText(image.replacement, QTextCharFormat());
    }
}

std::expected<void, core::Error> export_pdf(const QString& input_path, const QString& output_path) {
    QFile input(input_path);
    if (!input.open(QIODevice::ReadOnly)) {
        return std::unexpected(io_error("gui.export_input_unreadable", input_path, input.errorString()));
    }
    const QString text = QString::fromUtf8(input.readAll());

    QTextDocument document;
    if (QFileInfo(input_path).suffix().compare(QLatin1StringView("md"), Qt::CaseInsensitive) == 0) {
        // NoHTML: ham <img> etiketi görsel parçası değil düz metin olur. Görseller ayrıca strip_images'la
        // atılır.
        document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                                       QTextDocument::MarkdownNoHTML);
        strip_images(document);
    } else {
        document.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        document.setPlainText(text);
    }

    QSaveFile output(output_path);
    if (!output.open(QIODevice::WriteOnly)) {
        return std::unexpected(io_error("gui.export_output_unopenable", output_path, output.errorString()));
    }
    {
        QPdfWriter writer(&output);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageMargins(QMarginsF(kPageMarginMm, kPageMarginMm, kPageMarginMm, kPageMarginMm),
                              QPageLayout::Millimeter);
        // Belge yazıcının çözünürlüğünde ve kenar boşluğu içindeki alana göre sayfalanır. Sayfalanmamış
        // belgede QTextDocument::print kendi 2 cm kenarını ve sayfa numarasını ekler, 15 mm sözleşmesi
        // bozulurdu.
        document.documentLayout()->setPaintDevice(&writer);
        document.setPageSize(writer.pageLayout().paintRectPixels(writer.resolution()).size());
        document.print(&writer);
    }
    if (!output.commit()) {
        return std::unexpected(io_error("gui.export_output_unwritable", output_path, output.errorString()));
    }
    return {};
}

}  // namespace noteapp::gui
