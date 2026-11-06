#pragma once

// Not ve ilişki özellikleri (F4 "özellik denetçisi"; SPEC-graph "Özellikler"). Kullanıcı bir notun düğümüne
// ya da bir ilişkinin (`RELATED_TO`) kenarına anahtar/değer yazar, okur, siler. Yazma sorgu çubuğunun
// korumasından geçer (core/note_query.cpp SchemaGuard): uygulamanın türettiği anahtarlar yalnız okunur.
// İlişkinin `name` ve `directed`ı da burada salt okunurdur: adı ve yönü ilişki eylemleri değiştirir
// (rename/direction).
#include "core/error.hpp"
#include "core/note_store.hpp"
#include "graph/value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

// Denetçinin yazdığı değer türleri. Saklanan başka tür (zaman, bayt) yalnız gösterilir.
enum class PropertyType { Text, Number, Bool };

struct Property {
    std::string key;         // senkronla gelebilir: güvenilmez
    graph::Value value;      // büyük değerde null (large); boyutu size'da
    std::uint64_t size = 0;  // graph::PropertyHead::size
    bool large = false;      // değer yüklenmedi (gövde gibi)
    bool read_only = false;  // uygulamanın anahtarı: yazma reddedilir
    bool operator==(const Property&) const = default;
};

// Notun düğümündeki özellikler, anahtar sırasıyla; eşzamanlı head'lerden kazanan (LWW) gösterilir. Not yoksa
// NotFound.
std::expected<std::vector<Property>, Error> note_properties(const NoteStore& store, const NotePath& note);
// İlişkinin kenarındaki özellikler. Kenar yoksa, ilişki değilse ya da bir ucu canlı not değilse NotFound
// (error.relation_not_found).
std::expected<std::vector<Property>, Error> relation_properties(const NoteStore& store,
                                                                const RelationId& relation);

// Tek işlemde yazar; null değer (monostate) siler. Anahtar graph::detail::validate_name kuralıyla
// (error.property_key, reason), uygulamanın anahtarı error.property_reserved, ilişkinin adı ve yönü
// error.property_relation_reserved ile reddedilir (InvalidPath); hiçbir şey yazılmaz. Olmayan anahtarı silmek
// NotFound (error.property_not_found). Not ya da ilişki yoksa NotFound.
std::expected<void, Error> set_note_property(NoteStore& store, const NotePath& note, std::string_view key,
                                             const graph::Value& value);
std::expected<void, Error> set_relation_property(NoteStore& store, const RelationId& relation,
                                                 std::string_view key, const graph::Value& value);

// Yazılan metin -> değer. `type` verilmezse metin önekiyle (`text:`, `number:`, `bool:`) ya da kendiliğinden
// seçilir: `true`/`false` bool, tam tamsayı ya da sonlu ondalık sayı number, gerisi metin. Sayı tam metindir
// (boşluk yok). Geçersiz sayı ya da bool InvalidPath (error.property_value, reason not_number, not_bool).
std::expected<graph::Value, Error> parse_property(std::string_view text,
                                                  std::optional<PropertyType> type = std::nullopt);
// Zaman damgasının metni: UTC "YYYY-MM-DD HH:MM:SS.mmm"; 0000–9999 yılı dışında (senkronla gelebilir) ham
// `<time:N>`. Sorgu tablosu da bunu kullanır.
std::string timestamp_text(std::int64_t unix_ms);
// Değerin metni: bool true/false, tamsayı ondalık, ondalık sayı en kısa gidiş-dönüş biçimi (tamsayı görünürse
// `.0` eklenir: türü korunur), metin olduğu gibi, zaman UTC "YYYY-MM-DD HH:MM:SS.mmm", bayt `<bytes:N>`.
// Ham: arayüz temizler.
std::string property_text(const graph::Value& value);
// Düzenleme satırının metni: parse_property(property_input(v)) == v (metin, sayı, bool). Kendiliğinden başka
// türe okunacak ya da önekle başlayan metin `text:` öneki alır.
std::string property_input(const graph::Value& value);
// Denetçinin türü; zaman, bayt ve null'da nullopt.
std::optional<PropertyType> property_type(const graph::Value& value);
// "text", "number", "bool"; tersine çeviri bilinmeyende nullopt (CLI `--type`).
std::string_view property_type_name(PropertyType type);
std::optional<PropertyType> property_type_named(std::string_view name);

// `prop:` araması: anahtarı tam `key` olan özelliği taşıyan canlı notlar ve değerinin metni (property_text),
// yol sırasıyla. Görüntü başına anahtar başına bir kez okunur ve paylaşılır (arama her tuşta sorar). Depo
// yoksa boş.
using PropertyNotes = std::vector<std::pair<NotePath, std::string>>;
// defaults.toml `limits.search.property_keys`ın yapılandırmasız NoteStore'daki karşılığı; bir test ikisinin
// aynı olduğunu denetler.
inline constexpr std::size_t kDefaultPropertyCacheKeys = 32;
std::expected<std::shared_ptr<const PropertyNotes>, Error> notes_with_property(const NoteStore& store,
                                                                               std::string_view key);

}  // namespace noteapp::core
