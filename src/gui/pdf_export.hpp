#pragma once

#include "core/error.hpp"

#include <QString>
#include <expected>

class QTextDocument;

namespace noteapp::gui {

// UTF-8 metin dosyasını A4, 15 mm kenar boşluklu PDF'e yazar: `.md` Markdown olarak dizilir, diğerleri eş
// aralıklı düz metin. Çıktı atomik yazılır (yarım PDF kalmaz). Çağıran tarafta canlı bir QGuiApplication
// olmalıdır.
// Hata core::Error'dur (Code::Io, gui.export_* anahtarı, {path}/{detail}); metni core::format_error üretir.
std::expected<void, core::Error> export_pdf(const QString& input_path, const QString& output_path);

// Belgedeki her görsel parçayı alt metniyle (yoksa katalogdaki yer tutucuyla) değiştirir. Not içeriği
// güvenilmez (senkronla gelir): `file://sunucu/...` yolu Windows'ta UNC'ye gidip NTLM özeti sızdırır,
// `file:///...` yerel görseli PDF'e gömer. Hiçbir görsel kaynağı istenmesin diye düzen/print'ten ÖNCE
// çağrılır.
void strip_images(QTextDocument& document);

}  // namespace noteapp::gui
