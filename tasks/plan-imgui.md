# Plan: Qt'yi kaldır, GUI'yi SDL3 + Dear ImGui'ye taşı

> Durum: **ONAY BEKLİYOR** · Tarih: 2026-10-31 · Toolkit kararı kullanıcının ("imgui olsun").
> Onaylanınca SPEC.md §3, SPEC-gui.md, SPEC-cli.md gövdesi güncellenir, sonlarına `## Değişiklik 2026-10-31` eklenir;
> görevler `todo.md`'ye Faz 7 olarak geçer. İş `feat/imgui` dalında yapılır.

## Karar

| Bileşen | Bugün | Yeni | Lisans | Pin |
|---|---|---|---|---|
| Arayüz | Qt 6 Widgets | Dear ImGui | MIT | v1.93.0, FetchContent |
| Pencere, girdi, IME, pano, dosya diyaloğu, tema | Qt | SDL3 | zlib | release-3.4.18, FetchContent, statik |
| Çizim | Qt | SDL_Renderer (`imgui_impl_sdlrenderer3`) | zlib, MIT | yukarıdakilerle |
| PDF | `QPdfWriter` + `QTextDocument` | libharu + md4c + kendi yerleşim kodumuz | zlib, MIT | v2.4.6, v0.6.0 |
| Font (arayüz + PDF) | Sistem fontu | Gömülü DejaVu Sans + Sans Mono | Bitstream Vera (serbest) | sürüm T37'de sabitlenir |
| Windows derleyici | Qt'nin MinGW 13.1'i | MSYS2 MinGW-w64 GCC | GPL + runtime exception | sistem |

**Vulkan yok:** Not uygulaması 2B metin çizer. SDL_Renderer yeterlidir, sürücü yoksa yazılımsal çizime düşer.
**Dear ImGui Test Engine kullanılmaz:** Kendi lisansı (v1.04) OSI onaylı değil; "tamamen FOSS" şartını bozar.

## Mimari

ImGui anlık modlu (immediate mode) olduğundan Qt sınıfları birebir taşınmaz:

- **`GuiState` (saf model, ImGui/SDL bağımlılığı yok):** `TuiState` deseniyle not listesi, seçim, tampon, kirli durum, kaydetme,
  çakışma, klasör işlemleri ve ayar doğrulaması burada. Testlerin çoğu buraya yazılır.
- **`view` (ince çizim katmanı):** Her karede `GuiState`'i çizer, kullanıcı niyetini ona iletir. Diyaloglar ImGui modalıdır;
  dosya/klasör seçimi `SDL_Show*Dialog`.
- **`Deciders` dikişi:** Korunur, `QString` yerine `std::string` kullanır.
- **Olay döngüsü:** `SDL_WaitEventTimeout`; boştayken sürekli çizim yapılmaz. AutoSync sonucu `SDL_PushEvent` ile ana iş parçacığına geçer.
- **Başsız test:** Çizim testleri `SDL_VIDEO_DRIVER=dummy` ve yazılımsal çizimle koşar, tuşlar `io.AddKeyEvent` ile verilir. Xvfb gerekmez.

## Davranış değişiklikleri

1. **Ekran okuyucu desteği kalkar:** SPEC-gui "erişilebilir adlar" maddesi emekli olur. Klavyeyle tam kullanım korunur.
2. **Yerel görünüm kalkar:** Pencere KDE/Windows temasını izlemez; yalnız açık/koyu ayrımını `SDL_GetSystemTheme` ile izler.
3. **Editörde içerik vurgusu değişir:** `InputTextMultiline` metin renklendirmez. İçerik aramasında ilk eşleşmeye gidilir ve seçilir;
   diğer eşleşmeler boyanmaz.
4. **Arayüzde DejaVu'da olmayan glifler görünmez:** CJK ve renkli emoji `?` kutusu çıkar. Dosya baytları bozulmaz.
5. **`note export --pdf` artık `notes-gui` çağırmaz:** `note`, `noteapp_pdf`'e doğrudan link olur; `notes-gui --export-pdf` kalkar.
   SPEC-cli kabul 10 (`notes-gui` yoksa çıkış 3) emekli olur. GUI'siz derlemede ve ekransız sunucuda PDF çalışır.
6. **PDF yalnız soldan sağa basit metin dizer:** Arapça/İbranice ve karmaşık shaping yok. Türkçe tam desteklenir.
   Markdown kapsamı: başlık, kalın/italik, liste, kod, alıntı, bağlantı metni, GFM tablo. Görseller bugünkü gibi alt metne döner.
7. **`gui-state.ini` kalkar:** `gui-state.toml` başlar (toml++). Pencere boyutu ve son not bir kez sıfırlanır; not verisi etkilenmez.
8. **Geç NUL içeren not salt-okunur açılır (uygulamada eklendi):** `InputText` C dizgisi işler; dosyanın ilk 8 KB'ından sonra NUL varsa
   kuyruk kesilip diske yazılırdı. Veri kaybını önler. Aynı nedenle 2 GiB üstü not da salt-okunur açılır.

## Riskler

- **Büyük not performansı:** `InputTextMultiline` tamponu her karede işler. T39'da 1 MB notta tuş gecikmesi ölçülür; sınır aşılırsa
  kullanıcıya sorulur, gizli çözüm eklenmez.
- **Türkçe giriş ve kısayollar:** `ı/İ`, dead key ve Ctrl kısayolları Türkçe klavyede elle doğrulanır.
- **Linux dosya diyaloğu:** SDL3 portal veya zenity kullanır; ikisi de yoksa yol elle yazılabilen ImGui alanına düşülür.
- **Windows burada doğrulanamaz:** Bu makine Fedora. MSYS2 derlemesi, IME ve koyu tema iş makinende elle doğrulanır.
- **Taban ölçülemedi:** Bu makinede `ninja-build` ve `libsecret-devel` yok. Doğrulama `scripts/check-fedora.sh` ile konteynerde yapılır;
  Docker servisinin çalıştığı T37'de teyit edilir.

## Görevler

Sıralı bağımlılık zinciri; tek yazar, klasik dispatch. `tests/gui`'deki 108 `TEST_CASE`'in davranışı korunur; emekli olanlar T42'de gerekçesiyle listelenir.
Ekip: `cpp-developer`, `cpp-test-engineer`, `devops-specialist`, `documentation-specialist`; bağımsız review `cpp-reviewer`, `security-reviewer`.

### T37: Taban + bağımlılık spike'ı
- **Scope:** `cmake/Dependencies.cmake`, `docker/fedora.Containerfile`, `scripts/check-fedora.sh`, `src/gui/CMakeLists.txt`, `resources/fonts/**`
- **Kabul:** değişiklik öncesi taban konteynerde koşulur, sonucu yazılır · SDL3/ImGui/libharu/md4c pin + hash ile derlenir, libgit2'nin
  gömülü zlib'iyle çakışmaz · `çğıİöşü` çizen pencere `dummy` sürücüde açılıp kare üretir · `-DNOTEAPP_BUILD_GUI=OFF` çalışır.
- **Doğrulama:** `scripts/check-fedora.sh` → exit 0.

### T38: 🔴 `noteapp_pdf` (Qt'siz)
- **Depends:** T37 · **Scope:** `src/pdf/**`, `tests/pdf/**`, `src/cli/commands_export.cpp`, `src/cli/CMakeLists.txt`, `tests/cli/cli_export_test.cpp`
- **Kabul:** A4, 15 mm · Türkçe metin `pdftotext` ile geri okunur · uzun satır ve sayfa taşması doğru · görsel başvurusu ağ/dosya isteği açmaz
  (`strip_images` testlerinin karşılığı) · atomik çıktı, artık dosya yok · hatalı girdide çıkış 1.
- **Doğrulama:** `ctest -R "pdf|cli"` konteynerde → exit 0.

### T39: 🔴 `GuiState` + pencere + liste + editör + kaydetme
- **Depends:** T37 · **Scope:** `src/gui/{main.cpp,gui_state.*,view.*,save_dialog.*}`, `tests/gui/{window_fixture.hpp,main_window_test.cpp,note_list_model_test.cpp,save_test.cpp}`
- **Kabul:** SPEC-gui kabul 1–3, 7 · kısayol tablosu aynı · kaydetmede veri kaybı senaryolarının tamamı taşınır · CRLF, BOM ve salt-okunur kuralları korunur ·
  boşta CPU kullanımı ölçülür · 1 MB not ölçümü yazılır.
- **Doğrulama:** `ctest -R gui` konteynerde → exit 0.

### T40: Klasörler + içerik araması + dosya izleme
- **Depends:** T39 · **Scope:** `src/gui/{gui_state.*,view.*}`, `tests/gui/{folders_test.cpp,content_search_test.cpp}`
- **Kabul:** SPEC-gui kabul 4 · QFileSystemWatcher yerine `rescan_seconds` taraması + pencere odağına dönüşte tarama (SDL'de izleyici yok; yeni bağımlılık eklenmez) ·
  bağ kaçışı testleri korunur.
- **Doğrulama:** `ctest -R gui` konteynerde → exit 0.

### T41: Ayarlar + senkron + PDF diyaloğu
- **Depends:** T38, T40 · **Scope:** `src/gui/{settings_page.*,gui_state.*,view.*}`, `src/sync/auto_sync.hpp`, `tests/gui/{settings_test.cpp,pdf_test.cpp}`
- **Kabul:** SPEC-gui kabul 5–6 · token maskeli, config'e ve loga girmez · çalışan senkron gereksiz yere öldürülmez.
- **Doğrulama:** `ctest -R gui` konteynerde → exit 0.

### T42: Kaldırma + dağıtım + belge
- **Depends:** T41 · **Scope:** `src/gui/CMakeLists.txt`, `tests/gui/CMakeLists.txt`, `CMakeLists.txt`, `CMakePresets.json`, `scripts/**`, `README.md`, `SPEC*.md`, `tasks/todo.md`
- **Kabul:** `grep -rIE "\bQt|qt6|windeployqt|QT_QPA" . --exclude-dir=.git --exclude-dir=tasks` yalnız `## Değişiklik` kayıtlarını bulur · temiz `git clone` +
  `scripts/check-fedora.sh` yeşil · `cmake --install` üç ikiliyi kurar · Release'te SPEC §10/2–3 yeniden ölçülür.
- **Doğrulama:** komutlar + `cpp-reviewer`, `security-reviewer` bulguları kapalı.

### Elle (kullanıcı)
- Fedora KDE: Türkçe giriş, koyu tema, PDF göz kontrolü.
- Windows 11: MSYS2 derlemesi, Türkçe IME, koyu tema.
- Önceden açık olan: gerçek GitHub senkronu (HTTPS + SSH), Windows ↔ Fedora.
