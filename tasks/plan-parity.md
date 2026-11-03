# Plan: TUI–GUI eşitliği (tek çekirdek, iki ince arayüz)

> Durum: **ONAYLANDI** (kullanıcı 2026-11-03: "gui tui özelliklerini birebir karşılasın… bir arayüzde açıp diğerinde kapatma")
> Envanter: 98 özellik — ikisinde aynı 35, farklı 26, yalnız TUI 27, yalnız GUI 8, hiçbirinde 2. 16 yinelenen mantık (D1–D16).

## Lead kararları (davranış çakışmaları, güvenli olan seçildi)
1. Klasör silme: iki arayüzde de çöpe (geri alınabilir). Listede onaylı kalıcı silme (`D` / `Shift+Del`, varsayılan Vazgeç) iki arayüzde de kalır (kullanıcının önceki isteği).
2. Çöp geri alma: iki arayüzde de yığın (çok adım).
3. Otomatik kayıt: iki arayüzde de (`tuning.autosave_ms`).
4. Yeni not baytları: korunur (GUI'nin LF normalleştirmesi kalkar).
5. Uzantı ayırma: tek kural `core::split_typed_name` (SPEC-tui:221).
6. Ağaç sıralaması: iki arayüzde de tercih edilen sıralama + sıralama menüsü.
7. Dil değişimi: iki arayüzde de anında.
8. TUI harita: ortak yerleşim (`graph_layout` → `src/app`), FTXUI braille canvas ile çizim. TUI sekmeler: çok belgeli `EditorSession` + sekme satırı.
9. Araç çubuğu / bağlam menüsü: eylem eşitliği esastır, widget değil; TUI'de eylemler palet + ipucu + sağ tık menüsü (fare) ile.
10. SPEC "kapsam dışı" satırları (SPEC-gui:255 sözdizimi/csv/json, SPEC-tui:412 PDF) kaldırılır; çoklu imleç ve sürükle-bırak ikisinde de yok (eşit).

## Görevler (sıra; en çok 2 derleyen ajan; her görevde iki arayüz birlikte)
- **A0** `src/app` (`noteapp_app`, FTXUI/ImGui/SDL'siz) iskeleti + basit kopyaların taşınması (D1, D9, `kCopySuffix`, D4 yardımcıları).
- **A8** FTXUI'siz editör yığınını `noteapp_tui`'den ayır (`src/edit`: buffer, syntax, spell, completion, find, input_line, clipboard, child_process, lsp_*, display_text, key).
- **A1–A7** ortak modüller: commands/Intent, listing (filtre/sıralama/geçmiş), trash_ops, move/copy/link ops, document (kayıt/çakışma/salt okunur/otomatik kayıt), settings_model/sync_view/ui_prefs, preview_kind + csv/json.
- **R1–R4** TUI ve GUI ortak modülleri kullanır (kopyalar silinir); davranış kararları 1–7 burada uygulanır.
- **G0** `gui/view.cpp` (2808 satır) ve `gui_state.cpp` (2164) panellere bölünür (mekanik).
- **G1–G4** GUI eksikleri: arama (genel arama, geçmiş, tamamlama, `sync:`, görünüm seçici), liste/çöp (çoklu seçim, kopyala-yapıştır, klasör taşı/kopyala, sıralama menüsü, çöp önizleme), ilişki `L`/komut paleti/yardım/ortak kısayol tablosu, önizleme (CSV/JSON, dış editör, bayt/yol kopyala, başıboş dosya bildirimi).
- **G5a–c** GUI editörü `EditorBuffer` üstünde özel widget: satır no, durum çubuğu, not içi bul, geri al; sonra sözdizimi/yazım/tamamlama; sonra LSP.
- **T1–T4** TUI eksikleri: otomatik kayıt, PDF dışa aktarma, bağlantı testi; önizlemede `[[wiki]]` takibi; sekmeler; harita.

## Kabul
Her görev: iki arayüzde aynı davranışı kanıtlayan test (ortak modül testi + her arayüzde bir duman testi), önce düşen test, SPEC-tui/SPEC-gui eşitlik tablosu güncel, kaynak kuralları. Son: özellik matrisinde T/G/P satırı kalmaz.
