# Plan: haritada çizge veritabanı çalışma ortamı (Neo4j benzeri)

> Durum: **ONAYLANDI** (kullanıcı 2026-11-05: hepsi sırayla; yön varsayılanı yönsüz + "Yönlü" kutusu, ad başına hatırlanır; harita sorgu çubuğunda yazma sorguları da onay penceresiyle — önce Cypher yazmasının not türetmeleriyle (bağlantı, etiket, klasör) tutarlı kalması sağlanır) · İstek 2026-11-05: "map ekranından klavye ve fare ile bana graph db'nin (neo4j) tüm işlevselliğini ver"
> Kısıt: geriye uyum yok (kullanıcı 2026-11-05), migrasyon yazılmaz. Kaynak: cpp-architect tasarımı (bu dosyada özet).

## Model kararı
- İlişki = `RELATED_TO` kenarı + `name` (gösterim) + `directed` (bool) özelliği. Ad değişince kenar kimliği aynı kalır; birleşim LWW, çakışma raporlanır.
- Kenar kimliği uçlardan + ad anahtarından türer (yönsüzde uçlar sıralı): A→B ve B→A adsız iki kenar olmaz (bugünkü hata da düzelir).
- Bir çiftte farklı adlı birden çok ilişki olabilir, adsız en çok bir tane. Ad: kırpılır, geçerli UTF-8, denetim karakteri yok, uzunluk sınırı yok; eşleme `tag_key` (Türkçe İ/I).
- Seçici: `relation_names()` → ad, kullanım sayısı (sayı azalan).

## Fazlar
- **F1 adlı ilişki:** çekirdek API (`add/rename/remove_relation`, `NoteLinks::related` ad+yön), birleşim testleri, GUI bırakma/sağ tık "Bağla…" ad penceresi (süzülen liste + yeni ad + "Yönlü"), kenar menüsü (yeniden adlandır, yön çevir, kaldır), kenar etiketi; TUI `L` ve harita `C` ad satırı (Tab tamamlar); CLI `note relation add|rm|rename|ls`.
- **F2 keşif ve düğüm işlemleri:** komşuları türe/derinliğe göre aç-kapa, etiket ve ilişki adı süzgeci, iki düğüm arası en kısa yol (app katmanında BFS), sabitle/çöz, haritadan yeni not / çöpe at, ara-odaklan; TUI eşdeğeri.
- **F3 sorgu çubuğu:** haritada Cypher (okuma + onaylı yazma; yazma not türetmelerini tutarlı tutar), sonuçlar haritada vurgulu + tablo görünümü, kayıtlı görünümler; TUI tek satır sorgu + tablo.
- **F4 özellik denetçisi:** kenar ve not özellikleri (anahtar/değer) ekle/düzenle/sil; ayrılmış anahtarlar reddedilir; "label" = etiket.

## Kabul
Her faz: önce düşen test, GUI+TUI eşitliği (+ CLI), birleşim testleri, SPEC-graph/gui/tui, kaynak kuralları, review (+ security: güvenilmez ad metni).
