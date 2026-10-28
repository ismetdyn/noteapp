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

}  // namespace

std::expected<void, QString> export_pdf(const QString& input_path, const QString& output_path) {
    QFile input(input_path);
    if (!input.open(QIODevice::ReadOnly)) {
        // TODO(T09): i18n
        return std::unexpected(
            QStringLiteral("girdi okunamadı: %1: %2").arg(input_path, input.errorString()));
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
        // TODO(T09): i18n
        return std::unexpected(
            QStringLiteral("çıktı açılamadı: %1: %2").arg(output_path, output.errorString()));
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
        // TODO(T09): i18n
        return std::unexpected(
            QStringLiteral("çıktı yazılamadı: %1: %2").arg(output_path, output.errorString()));
    }
    return {};
}

}  // namespace noteapp::gui
