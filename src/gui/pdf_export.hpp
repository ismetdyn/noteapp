#pragma once

#include <QString>
#include <expected>

namespace noteapp::gui {

// UTF-8 metin dosyasını A4, 15 mm kenar boşluklu PDF'e yazar: `.md` Markdown olarak dizilir, diğerleri eş
// aralıklı düz metin. Çıktı atomik yazılır (yarım PDF kalmaz). Çağıran tarafta canlı bir QGuiApplication
// olmalıdır.
// Hata metni core::tr() ile yerelleştirilmiş QString'tir. TODO(T05): hata tipi core::Error olacak.
std::expected<void, QString> export_pdf(const QString& input_path, const QString& output_path);

}  // namespace noteapp::gui
