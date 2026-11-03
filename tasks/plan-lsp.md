# Plan: LSP istemcisi (T59)

> Durum: **ONAYLANDI** (2026-11-02) · Varsayılan kapalı · Kaynak: cpp-architect planı
> Kullanıcı kararları: diagnostics **+ completion**; Windows **ilk sürümde**; notlar **geçici dizine yazılır** (import çözülsün).
> Lead kararı: JSON parser genişletmesinin import yan etkisi (bilinmeyen alanda `null`) kabul.
>
> Bu kararlarla değişenler (aşağıdaki "v1 kapsamı" ve "Belge" maddelerini ezer):
> - Completion v1'de: LSP önerileri mevcut popup'a eşzamansız eklenir (T59c'ye +~5 s).
> - Çalışma alanı: `state_dir/lsp/workspace/` (0700) altında, sunucusu tanımlı dillerin notları 0600 dosya olarak yansıtılır; kaydet/senkron/yeniden adlandır/sil ile güncellenir, `Env`/`Properties` notları asla yazılmaz; kök bu dizindir. Çıkışta silinmez (bir sonraki açılışta yeniden eşitlenir).
> - T59d (Windows) zorunlu.
> - 2026-11-02 kullanıcı: LSP **varsayılan açık**, varsayılan sunucular python (pyright), c/cpp (clangd), sh, markdown, lua; daha fazla dosya türü eşlemesi. Proje kodu çalıştıran ya da ağdan şema indiren sunucular yalnız yorumlu örnek. Kurulu olmayan sunucu sessiz geçilir, notları diske yazılmaz.
> - 2026-11-02 kullanıcı: güvenlik review'u (senkronlu not → proje dosyası → kod çalıştırma) için "tüm notlar yansıtılsın + README uyarısı" seçildi.

## v1 kapsamı
- Yalnız TUI, yalnız **diagnostics**: hatalar altı çizili, imleçteki mesaj editör mesaj satırında. 250 ms boşta tam metin `didChange` (spellcheck idle kalıbı).
- Kapsam dışı: completion (mevcut popup senkron, `editor_session.cpp:549-558`), hover, format, goto/rename, GUI (`InputTextMultiline` aralık çizemez), 2 MiB üstü not.
- `SPEC-tui.md:388` "LSP kapsam dışı" satırı güncellenir.

## Tasarım
- **Belge:** `file://<state_dir>/lsp/<NotePath>` URI, kök boş `state_dir/lsp/` (0700). İçerik diske yazılmaz, `didOpen/didChange` ile gider. Tek dosya modu.
- **Dil:** `syntax_language_for(ext)` (`syntax.hpp:71`) → languageId. `Env`/`Properties` hiç gönderilmez (sır).
- **Süreç:** `config.toml` `[lsp] enabled=false`, `[lsp.servers] python=["pyright-langserver","--stdio"]`. Kabuksuz, otomatik indirme yok. Dil başına tembel `LspClient`, `SpellChecker` kalıbı (`jthread` son üye). Çökme: o dil oturumda kapanır.
- **Güvenlik:** komut yalnız yerel config'ten; ortamdan token düşer; sunucu metni ≤1000 tanı, 512 bayt, `terminal_safe`; Content-Length başlık ≤4 KiB, gövde ≤16 MiB; logda içerik yok.
- **JSON:** yeni bağımlılık yok. `cli/json_parser.hpp` + `json_text.hpp` → `src/core/json*`, RFC 8259'a tamamlanır (null, ondalık, kaçışlar, surrogate, ayarlı derinlik). Yan etki: import, bilinmeyen alanda `null` taşıyan satırı kabul eder.

## Görevler (~18 saat ajan)
- **T59a** protokol çekirdeği + JSON (3 s)
- **T59b** `LspClient` + sahte LSP sunucusu test aracı (5 s)
- **T59c** config + TUI entegrasyonu (5 s)
- **T59d** Windows pipe'lı `ChildProcess` (5 s, isteğe bağlı)

## Kullanıcı kararları
1. v1'de diagnostics yeterli mi, completion da mı (+~5 s)?
2. Windows v1'e girsin mi (T59d)?
3. Tek dosya modu yeterli mi (notlar arası import yok)?
