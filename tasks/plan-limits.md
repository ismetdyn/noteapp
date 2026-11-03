# Plan: Sabit sınır yok ("sınırsız dünya")

> Durum: **ONAYLANDI** (kullanıcı isteği 2026-11-03) · Envanter: codebase-explorer raporu (A 51, B 31, C 20 satır; D kapsam dışı)
> Kural: kullanıcı işlemi tavanla reddedilmez (parti/akış/sanallaştırma); kalan her sayı config'te (`resources/defaults.toml`), kapasite için `0 = sınırsız`; güvenilmez girdi korumaları kalır ama değeri config'ten gelir, sınırsız yapılırsa riski yorumda yazılır.

## Görevler (sırayla; aynı anda en çok 2 derleyen ajan)

- **L0 — Config sözleşmesi (önce, tek yazar):** `[limits]` bölümü ve tipli erişim (`core::Limits`), `0 = sınırsız` gösterimi (`positive_integer` 0'ı reddediyor → `limit_value`), birim ekleri (`64MiB`, `5s`) isteğe bağlı, doğrulama ve uyarı yolu, `save_config` round-trip, `note config list/set` desteği, SPEC-core. Tüm modüller bundan sonra anahtar ekler. Tek olgu: aynı değerin kopyaları (A27 `kContentResultLimit` x2, A41 dosya adı sınırları x3, A43 ad deneme sigortaları x3) tek sahibe toplanır.
- **L1 — GUI (A1–A4):** sekme sınırı kalkar (sekme çubuğu kaydırma zaten var), harita düğüm tavanı yerine görünür alan sanallaştırma/LOD + config, derinlik sınırı config, önizleme parça parça yerleşim (256 KiB tavanı kalkar).
- **L2 — TUI (A5–A26, A31, A32, A47, A48, A51):** biçimli görünüm/tablo/sözdizimi renklendirme görünür pencere ile tembel üretim; tamamlama, geçmiş, geri alma, yapıştırma sınırları config (`0 = sınırsız`); OSC52 terminal protokol sınırı kalır (yalnız ileti).
- **L3 — Core/arama (A27–A30, A33, A35–A45, A50):** not başına bağlantı/etiket sınırı yerine işlem bölme; çakışma kopyası sayacı yerine benzersiz sonek; klasör derinliği config; arama sonuçları sayfalı/sanallaştırılmış; `max_note_bytes` 0 = sınırsız.
- **L4 — Graph/sync/sorgu (B1–B21, C10):** kullanıcı sorgusu ile senkronla gelen görünüm sorgusu ayrı limit profili (görünüm korumalı, kullanıcı `0`); `QueryLimits`/`IngestLimits`/merge/HLC/peer değerleri config; toplu işlem sabitleri çerçeve sınırından türetilir (tek sahip).
- **L4b — Büyük not (B1 + A34):** 64 MiB çerçeve tavanı tek not boyutunu da sınırlıyor. Çözüm: değeri parçalara bölen oplog biçimi (chunked value). **Kullanıcı onayladı (2026-11-03):** parçalı biçim; eski sürüm cihaz bilinmeyen çerçeveyi görünce senkronu durdurur ve güncelleme ister, veri kaybı yok.
- **L5 — LSP (B22–B28, C3):** değerler config'e (`[lsp.limits]`), sınırsız seçenekli.
- **L6 — Son tarama:** çıplak sayı literalleri (`rg '\b[0-9]{3,}\b'`) modül modül; kalan sınırlar ya config ya gerekçeli sabit (OS/protokol).

## Lead kararları (T92b'nin "ikisi de" dediği sınırlar, 2026-11-03)
1. 60–64 MiB not gövdesi → L4b parçalı değer.
2. `note graph import` tek satır >65.536 op → satır içi parti (idempotent), L4.
3. `note query --write` >65.536 op → L4b: bir işlem birden çok çerçeveye yayılabilen atomik grup (çerçeve tavanı işlem tavanı olmaktan çıkar).
4. Not başına 1024 bağlantı / 256 etiket → türetme işlemlere bölünür, sessiz atlama yok, L3.
Çerçeve başına 65.536 op ve 64 MiB yalnız tek çerçeve (ağ/disk birimi) korumasıdır; değerleri config'e (L4), kullanıcı işlemi asla bu yüzden reddedilmez.

## Kabul
Her görevde: önce düşen test ("sınırın üstünde işlem başarılı"), sınırın kaldırıldığını kanıtlayan ölçüm (bellek/süre), config round-trip testi, SPEC güncellemesi, kaynak kuralları (`--memory=8g --cpus=6`, `-j4`).
