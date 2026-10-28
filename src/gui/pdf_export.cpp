#include "gui/pdf_export.hpp"

#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTextDocument>

namespace noteapp::gui {

namespace {

constexpr qreal kPageMarginMm = 15.0;

// Katalog metni {path} ve {detail} ile doldurulur (core::format_error).
core::Error io_error(const char* key, const QString& path, const QString& detail) {
    return {core::Error::Code::Io, key, {{"path", path.toStdString()}, {"detail", detail.toStdString()}}};
}

}  // namespace

std::expected<void, core::Error> export_pdf(const QString& input_path, const QString& output_path) {
    QFile input(input_path);
    if (!input.open(QIODevice::ReadOnly)) {
        return std::unexpected(io_error("gui.export_input_unreadable", input_path, input.errorString()));
    }
    const QString text = QString::fromUtf8(input.readAll());

    QTextDocument document;
    if (QFileInfo(input_path).suffix().compare(QLatin1StringView("md"), Qt::CaseInsensitive) == 0) {
        document.setMarkdown(text);
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
