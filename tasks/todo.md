# Görev Listesi: noteapp v1

> Plan: [plan.md](plan.md) · Spec: [SPEC.md](../SPEC.md) · Durum: **ONAYLANDI (2026-10-28)**
>
> **Kısaltmalar (Windows, Git Bash/PowerShell):**
> `B` = `cmake --build --preset windows-mingw-debug` ·
> `T(x)` = `ctest --preset windows-mingw-debug -R x --output-on-failure` ·
> `F` = `scripts/check-fedora.sh` (Docker'da Fedora derleme + tüm testler)
>
> **Her görevin ortak "bitti" tanımı:** B uyarısız geçer · ilgili T(x) yeşil · `clang-format` uygulanmış ·
> yeni kullanıcı metni `en.toml` + `tr.toml`'da · kodda Türkçe tanımlayıcı yok.
> 🔴 = kritik yol: önce kırmızı test (TDD Demir Kural), her edge-case ayrı test.

---

## Faz 0 — Temel ve risk spike'ları

### T01: Derleme iskeleti
- [x] **Açıklama:** Kök CMake (C++23, `NOTEAPP_BUILD_GUI`/`NOTEAPP_BUILD_TESTS`, bizim hedeflere uyarı fonksiyonu),
  `CMakePresets.json` (windows-mingw, linux-gcc, macos-clang × debug/release + test preset'leri),
  `cmake/Dependencies.cmake` (doctest v2.5.3, toml++ v3.4.0), `.clang-format`, `noteapp_core` yer tutucu + ilk test.
- **Kabul:** temiz dizinde configure + build + ctest yeşil · `std::expected` kullanan test GCC 13'te derlenir ·
  üçüncü parti uyarıları build'i kırmaz · `scripts/dev-env.sh` Windows ortamını kurar.
- **Doğrulama:** `cmake --preset windows-mingw-debug && B && T(core)`
- **Bağımlılık:** yok · **Dosyalar:** `CMakeLists.txt`, `CMakePresets.json`, `cmake/Dependencies.cmake`,
  `.clang-format`, `src/core/CMakeLists.txt`, `tests/core/smoke_test.cpp` · **Boyut:** M

### T02: Fedora Docker doğrulaması
- [x] **Açıklama:** `docker/fedora.Containerfile` (gcc-c++, cmake, ninja-build, qt6-qtbase-devel, libsecret-devel,
  openssl-devel, pkgconf) + `scripts/check-fedora.sh` (repo'yu bağlar, `linux-gcc-debug` derler, ctest koşar,
  `QT_QPA_PLATFORM=offscreen`).
- **Kabul:** `F` T01 testlerini Fedora'da yeşil koşar · imaj önbelleklenir (ikinci koşu bağımlılık indirmez).
- **Doğrulama:** `F` · **Bağımlılık:** T01 · **Dosyalar:** 2 · **Boyut:** S · **Gerekli:** Docker Desktop çalışıyor

### T03: Spike — libgit2 + keychain
- [x] **Açıklama:** Dependencies'e libgit2 v1.9.7 (`USE_SSH=exec`, Windows `USE_HTTPS=WinHTTP`, Linux OpenSSL,
  test/CLI kapalı, gömülü zlib) ve keychain v1.3.1. Spike testi: geçici dizinde repo init → commit → yerel bare'e
  push → klonla. Keychain gidiş-dönüş testi `[manual]` etiketli (varsayılan koşuda yok).
- **Kabul:** Windows MinGW ve Fedora'da derlenir, spike testi yeşil · elle: `[manual]` keychain testi Windows'ta
  geçer · elle: SSH-exec ile GitHub'a `ls-remote` sonucu SPEC.md §12/2'ye yazılır.
- **Doğrulama:** `B && T(sync)`, `F` · **Bağımlılık:** T01, T02 · **Dosyalar:** `cmake/Dependencies.cmake`,
  `src/sync/CMakeLists.txt`, `tests/sync/libgit2_spike_test.cpp`, `SPEC.md` · **Boyut:** M · **Model:** opus
- **Açık:** Fedora derlemesi (T02/Docker yok) · GitHub'a SSH-exec `ls-remote` host anahtarı onayına takıldı → kontrol noktası 4.

### T04: Spike — FTXUI + Qt
- [x] **Açıklama:** FTXUI v7.0.3 ekle. `notes` yer tutucu: Türkçe örnek satırlarla sabit iki panel. `notes-gui`
  yer tutucu: pencere + `--export-pdf` ile "çğıİöşü" içeren PDF. Windows'ta konsol UTF-8 ayarı.
- **Kabul:** iki ikili Windows'ta açılır, Türkçe karakterler doğru görünür (elle) · PDF'te Türkçe glifler (elle) ·
  Fedora'da offscreen PDF testi yeşil · `NOTEAPP_BUILD_GUI=OFF` ile Qt'siz derleme çalışır.
- **Doğrulama:** `B && T(gui)`, `F`, elle çalıştırma · **Bağımlılık:** T01, T02 · **Dosyalar:** `cmake/Dependencies.cmake`,
  `src/tui/{CMakeLists.txt,main.cpp}`, `src/gui/{CMakeLists.txt,main.cpp}`, `tests/gui/pdf_smoke_test.cpp` ·
  **Boyut:** M · **Model:** opus
- **Not:** Windows B + T + GUI=OFF + PDF (Türkçe/A4/15 mm) doğrulandı; açık: `F` (T02) ve iki ikilinin elle göz kontrolü.

### ✅ Kontrol noktası 0
- [x] Windows (B + tüm testler) ve `F` yeşil
- [x] SSH-exec kararı SPEC'e işlendi; libssh2 gerekiyorsa kullanıcıya soruldu
- [x] Spike kodu atılabilir/yeniden kullanılabilir kısımları ayrıldı

---

## Faz 1 — core + ilk dikey dilim `note "metin"`

### T05: 🔴 `Error` + `NotePath::parse`
- [x] **Kabul:** SPEC-core "Yol güvenliği" ret kurallarının her biri ayrı testte reddedilir · geçerli iç içe yollar
  kabul edilir · hata `code` + i18n anahtarı taşır.
- **Doğrulama:** `B && T(core)` · **Bağımlılık:** T01 · **Dosyalar:** `src/core/{error.hpp,note_path.hpp,note_path.cpp}`,
  `tests/core/note_path_test.cpp` · **Boyut:** S

### T06: UTF-8 yardımcıları + transliterasyon + `slugify`
- [x] **Kabul:** `slugify("# Toplantı Notları: Q4!") == "toplanti-notlari-q4"` · boş/sembol girdi zaman damgalı ad ·
  60 karakter sınırı tire sınırında · bozuk UTF-8 çökertmez.
- **Doğrulama:** `B && T(core)` · **Bağımlılık:** T01 · **Dosyalar:** `src/core/{text.hpp,text.cpp}`,
  `tests/core/text_test.cpp` · **Boyut:** S

### T07: 🔴 `NoteStore` — oluştur/oku/listele/yaz
- [x] **Kabul:** ad çakışmasında `-2` (ezme yok) · atomik yazma: hata simülasyonunda orijinal aynen, `.tmp` kalmaz ·
  dış değişiklikte `ModifiedExternally` · sembolik bağla kaçış reddi · gizli dosya atlanır, ikili işaretlenir,
  mtime azalan · BOM korunur.
- **Doğrulama:** `B && T(core)`, `F` · **Bağımlılık:** T05, T06 · **Dosyalar:** `src/core/{note_store.hpp,note_store.cpp}`,
  `tests/core/note_store_test.cpp` · **Boyut:** M

### T08: Kaynak gömme + config + platform dizinleri
- [x] **Kabul:** `resources/defaults.toml` ikiliye gömülü · `config.toml` yoksa varsayılanlar · bozuk dosya → varsayılan
  + uyarı (çökme yok) · `~` genişletme · atomik kayıt · `NOTEAPP_HOME` tüm dizinleri ezer.
- **Doğrulama:** `B && T(core)` · **Bağımlılık:** T07 · **Dosyalar:** `cmake/EmbedResources.cmake`, `resources/defaults.toml`,
  `src/core/{config.hpp,config.cpp,paths.hpp,paths.cpp}`, `tests/core/config_test.cpp` · **Boyut:** M

### T09: i18n + log
- [x] **Kabul:** `tr("key", {args})` yer tutucuları doldurur · seçili dil → en → anahtar geri düşüşü · `en`/`tr` anahtar
  eşliği testi · `language=auto` Windows UI dili / `LANG`'dan · log 1 MB'ta döner.
- **Doğrulama:** `B && T(core)` · **Bağımlılık:** T08 · **Dosyalar:** `resources/i18n/{en,tr}.toml`,
  `src/core/{i18n.hpp,i18n.cpp,log.hpp,log.cpp}`, `tests/core/i18n_test.cpp` · **Boyut:** M

### T10: CLI hızlı ekleme dilimi
- [x] **Açıklama:** `note METİN…`, stdin, `-t/-f/-n`, `--no-sync` (şimdilik no-op), `--`, `--help`, `--version`,
  çıkış kodları, `wmain` + UTF-8 konsol. Test yardımcısı: ikiliyi `NOTEAPP_HOME` ile alt süreçte çalıştırma.
- **Kabul:** SPEC-cli kabul 1 ve 3 · `note -- ls` "ls" notu ekler · Türkçe argüman bozulmaz · çıktı i18n.
- **Doğrulama:** `B && T(cli)`, `F` · **Bağımlılık:** T09 · **Dosyalar:** `src/cli/{CMakeLists.txt,main.cpp,args.hpp,args.cpp}`,
  `tests/support/run_process.hpp`, `tests/cli/cli_add_test.cpp` · **Boyut:** M

### ✅ Kontrol noktası 1
- [x] Tüm testler Windows + `F` yeşil
- [ ] Elle: `note "merhaba dünya"` → `~/notes/merhaba-dunya.md`; süre ≤ 200 ms
- [ ] Kullanıcı ilk dilimi denedi

---

## Faz 2 — CLI'nin geri kalanı

### T11: detect modülü + korpus + CLI'ye bağlama
- [x] **Kabul:** SPEC-detect kabul 1–5 (korpusta ≥ %90, zorunlu örnekler, tek satır → varsayılan, 64 KB ≤ 5 ms Release) ·
  CLI çok satırlı girdide öneriyi kullanır, `-t` öneriyi ezer, çıktıda "önerilen/seçilen".
- [x] modül + korpus tamam; CLI bağlantısı T10 sonrası
- **Doğrulama:** `B && T(detect) && T(cli)` · **Bağımlılık:** T10 · **Dosyalar:** `src/detect/{CMakeLists.txt,detect.hpp,detect.cpp}`,
  `tests/detect/detect_test.cpp`, `tests/detect/samples/**` (veri), `src/cli/main.cpp` · **Boyut:** M

### T12: `core::process` + editör çözümleme/başlatma
- [x] **Kabul:** kabuksuz başlatma (CreateProcessW / posix_spawnp), bekleme, çıkış kodu · editör komutu tırnaklı
  argümanlarla bölünür · çözüm sırası config → VISUAL → EDITOR → platform · bulunamazsa denenen adaylar mesajda ·
  `fake-editor` test yardımcısı.
- **Doğrulama:** `B && T(core)`, `F` · **Bağımlılık:** T09 · **Dosyalar:** `src/core/{process.hpp,process.cpp,editor.hpp,editor.cpp}`,
  `tests/support/fake_editor.cpp`, `tests/core/editor_test.cpp` · **Boyut:** M

### T13: CLI editör akışı
- [x] **Kabul:** SPEC-cli kabul 4 · kayıt başarısızsa geçici dosya korunur ve yolu yazılır.
- **Doğrulama:** `B && T(cli)`; elle `EDITOR=micro note`, `EDITOR=edit note` · **Bağımlılık:** T11, T12 ·
  **Dosyalar:** `src/cli/main.cpp`, `src/cli/commands.cpp`, `tests/cli/cli_editor_test.cpp` · **Boyut:** S

### T14: 🔴 Taşı/sil/klasör + `note rm`/`note mv`
- [x] **Kabul:** SPEC-core kabul 8 · `.keep` ile boş klasör · SPEC-cli kabul 5 ve 7 (fuzzy ile silme yok, TTY'siz `-y` şart,
  hedef varsa ezmez).
- **Doğrulama:** `B && T(core) && T(cli)` · **Bağımlılık:** T13 · **Dosyalar:** `src/core/note_store.{hpp,cpp}`,
  `tests/core/note_store_ops_test.cpp`, `src/cli/commands.cpp`, `tests/cli/cli_manage_test.cpp` · **Boyut:** M

### T15: Fuzzy arama + `note ls/show/edit`
- [x] **Kabul:** SPEC-search kabul 1–3 · `rank_notes` 5.000 yolda ≤ 20 ms (Release) · `ls`, `show`, `edit` en iyi eşleşmeyi
  kullanır, seçilen yol stderr'e yazılır, eşleşme yoksa çıkış 1.
- **Doğrulama:** `B && T(search) && T(cli)` · **Bağımlılık:** T14 · **Dosyalar:** `src/search/{CMakeLists.txt,fold.hpp,fold.cpp,fuzzy.hpp,fuzzy.cpp}`,
  `tests/search/fuzzy_test.cpp`, `src/cli/commands.cpp` · **Boyut:** M
- [x] modül tamam; CLI bağlantısı T10 sonrası

### T16: `ContentIndex` + `note find`
- [x] modül tamam; CLI bağlantısı T10 sonrası
- [x] **Kabul:** SPEC-search kabul 4–6 · `find` çıktısı `yol:satır: kesit`, TTY'de vurgulu.
- **Doğrulama:** `B && T(search) && T(cli)` · **Bağımlılık:** T15 · **Dosyalar:** `src/search/{content_index.hpp,content_index.cpp}`,
  `tests/search/content_index_test.cpp`, `src/cli/commands.cpp`, `tests/cli/cli_search_test.cpp` · **Boyut:** M

### T16b: `NoteStore::list()` ikili bayrak önbelleği (T16'da bulundu)
- [x] **Açıklama:** `list()` her notun ilk 8 KB'ını okuyor: 5.000 notta ~540 ms (Release) → TUI açılışı (≤ 500 ms) ve
  değişiklik yokken `ContentIndex::refresh` (≤ 100 ms) hedeflerini bozuyor. `binary` bayrağı (mtime, boyut) anahtarıyla
  `NoteStore` içinde önbelleklenir; yalnız yeni/değişen dosyanın başı okunur. `content_index_test`'teki gevşetilmiş
  `refresh` bütçesi (yalnız ContentIndex payı) özgün kabule (toplam ≤ 100 ms) geri alınır.
  **Sonuç:** önbellek dosya açmayı kaldırdı (sıcak `list()` ~140–330 ms, soğuk ~540–750 ms; kurumsal Windows'ta dosya başına
  stat tabanı baskın, salt dizin gezintisi ~60–90 ms). ≤ 80 ms / toplam ≤ 100 ms için yerel dizin taraması gerekir → gevşetilmiş
  bütçe `ponytail:` notuyla korundu. **T16c ile kabul tuttu:** sıcak `list()` ~15–45 ms, özgün bütçe testte geri alındı.
- **Kabul:** değişiklik yokken `list()` 5.000 notta ≤ 80 ms (Release) · değişen dosyanın bayrağı güncellenir · testler yeşil.
- **Doğrulama:** `B && T(core) && T(search)`, Release ölçümü · **Bağımlılık:** T10 · **Dosyalar:** `src/core/note_store.{hpp,cpp}`,
  `tests/core/note_store_test.cpp`, `tests/search/content_index_test.cpp` · **Boyut:** S

### T16c: 🔴 Windows'ta hassas mtime + yerel dizin taraması (T16b'den sonra bulundu)
- [x] **Açıklama:** MinGW libstdc++ `last_write_time` saniyeye keser: okumayla aynı saniyedeki aynı boyutlu dış düzenleme
  `write()`'ta yakalanmıyor, üzerine yazılıyordu (veri kaybı); ikili bayrak önbelleği ve `ContentIndex::refresh` de kaçırıyordu.
  `core::stat_file` (Windows `GetFileAttributesExW` / FILETIME 100 ns, POSIX `lstat` / `st_mtim`) tek mtime kaynağı; `list()`
  Windows'ta `FindFirstFileExW` ile tarar (dosya başına stat yok, junction'a inilmez).
  **Sonuç (Release, 5.000 not, gürültülü kurumsal makine):** sıcak `list()` ~170–840 → ~13–50 ms; değişiklik yokken `refresh`
  ~175–1.000 → ~15–50 ms (özgün kabul testte geri alındı: toplam ≤ 100 ms, `list()` ≤ 80 ms). Soğuk `list()` (yeni süreç, ≈ TUI
  açılışı) ~430–1.800 → ~300–900 ms: ≤ 500 ms her seferinde tutmuyor; tabanı ikili bayrak için 5.000 dosyayı açmak (stat değil).
- **Kabul:** 50 ms arayla iki yazım `read()`/`list()`'te farklı mtime · aynı saniye + aynı boyut dış düzenlemede `write` →
  `ModifiedExternally`, dış içerik korunur · ikili bayrak ve `refresh` bu değişikliği görür · `list()` = `read()` = `write()` =
  `stat_file()` mtime · testler yeşil.
- **Doğrulama:** `B && T(core) && T(search)`, Release ölçümü · **Bağımlılık:** T16b · **Dosyalar:**
  `src/core/{file_stat.hpp,file_stat.cpp,note_store.cpp}`, `tests/core/note_store_test.cpp`, `tests/search/content_index_test.cpp` ·
  **Boyut:** S

### T16d: 🔴 Windows junction'ları not kökünden kaçırıyor (T16c'de bulundu)
- [x] **Açıklama:** MinGW libstdc++ junction'ı bağ saymaz (`is_symlink` false, `weakly_canonical` çözmez,
  `recursive_directory_iterator`/`remove_all` içine iner); junction yetki istemeden kurulur (`mklink /J`). `resolve()`
  `link/x.md`'yi kök içi sayıyordu → kök dışına okuma/yazma/oluşturma/taşıma; `remove_folder(force)` ve hatta `.keep`'li
  dış klasörde force'suz silme kök dışındaki dosyaları siliyordu. Sembolik bağ testleri Windows'ta atlandığı için
  yakalanmamıştı. **Düzeltme:** `resolve()` kökün altındaki var olan her bileşeni `stat_file` ile denetler, bağ →
  `InvalidPath`/`link` (her platformda); `fs::is_symlink` korumaları `stat_file` oldu; `remove_folder` önce bağ izlemeyen
  tarama (bağ → `contains_link`, hiçbir şey silinmez), sonra bağ izlemeyen alttan üste silme (`remove_all` yok);
  `folders()` junction'a inmez.
- **Kabul:** junction (POSIX'te sembolik bağ) üzerinden read/write/create/move/remove/create_folder/rename_folder →
  `InvalidPath` (`link`), dış dosya değişmez · içinde bağ olan klasörde `remove_folder` (force'lu/force'suz) →
  `contains_link`, ne dış hedef ne kök içi silinir · `list()`/`folders()` bağın ardını göstermez · testler atlanmaz, yeşil.
- **Doğrulama:** `B && T(core)` · **Bağımlılık:** T16c · **Dosyalar:** `src/core/{note_store.hpp,note_store.cpp}`,
  `tests/core/{note_store_test.cpp,note_store_ops_test.cpp,i18n_test.cpp}`, `resources/i18n/{en,tr}.toml`, `SPEC-core.md` ·
  **Boyut:** S

### T17: `note config` komutları
- [x] **Kabul:** `path|list|get|set` çalışır · geçersiz anahtar/değer çıkış 2 · SPEC-cli kabul 9 (dil değişimi).
- **Doğrulama:** `B && T(cli)` · **Bağımlılık:** T16 · **Dosyalar:** `src/cli/commands.cpp`, `src/core/config.cpp`,
  `tests/cli/cli_config_test.cpp` · **Boyut:** S

### ✅ Kontrol noktası 2
- [x] Tüm testler Windows + `F` yeşil
- [ ] Elle: `edit` ve `micro` ile yeni not/düzenleme; `note ls`, `note find` Türkçe sorgularla
- [ ] Kullanıcı CLI'yi denedi

---

## Faz 3 — TUI

### T18: `TuiState` saf modeli
- [x] **Kabul:** sorgu düzenleme → filtreli liste · gezinme (↑↓, Ctrl-P/N) · Esc mantığı (temizle/çık) · eşleşme yokken
  Enter → "yeni not" niyeti · ad/içerik modu geçişi — hepsi UI'siz birim testli.
- **Doğrulama:** `B && T(tui)` · **Bağımlılık:** T16 · **Dosyalar:** `src/tui/{tui_state.hpp,tui_state.cpp}`,
  `tests/tui/tui_state_test.cpp` · **Boyut:** S

### T19: FTXUI görünümü
- [x] **Kabul:** iki panel + üst sorgu/durum + alt kısayol çubuğu · eşleşme vurgusu · önizleme kaydırma · 60×15 altı
  mesajı · metne render testleri (ilk liste, filtre, küçük pencere) · 5.000 notla açılış ≤ 500 ms.
- **Doğrulama:** `B && T(tui)`, elle `notes` · **Bağımlılık:** T18 · **Dosyalar:** `src/tui/{CMakeLists.txt,main.cpp,view.hpp,view.cpp}`,
  `tests/tui/render_test.cpp` · **Boyut:** M

### T20: İçerik modu
- [x] **Kabul:** Tab ile içerik araması, eşleşme sayıları listede, önizleme ilk eşleşmeye kayar ve vurgular (render testi).
- **Doğrulama:** `B && T(tui)` · **Bağımlılık:** T19 · **Dosyalar:** `src/tui/{tui_state.cpp,view.cpp}`, testler · **Boyut:** S

### T21: Editör entegrasyonu + kaydet diyaloğu
- [x] **Kabul:** Enter aç / Ctrl-O yeni / eşleşmesiz Enter → sorgu ilk satır · editör `WithRestoredIO` ile askı-dönüş ·
  kaydet diyaloğu (ad slug, klasör fuzzy önerili, tip `detect` önerili + ←→ döngü) · Esc → "atılsın mı?" onayı.
- **Doğrulama:** `B && T(tui)`; elle `edit`/`micro` ile Windows Terminal'de · **Bağımlılık:** T20 ·
  **Dosyalar:** `src/tui/{save_dialog.hpp,save_dialog.cpp,view.cpp,tui_state.cpp}`, `tests/tui/save_dialog_test.cpp` · **Boyut:** M

### T22: Taşı/tip değiştir/sil + yardım
- [x] **Kabul:** Ctrl-R aynı diyalogla `move` (hedef varsa hata) · Ctrl-D onaylı, varsayılan "Vazgeç" · F1 yardım ekranı.
- **Doğrulama:** `B && T(tui)` · **Bağımlılık:** T21 · **Dosyalar:** `src/tui/{view.cpp,tui_state.cpp,confirm_dialog.cpp}`,
  testler · **Boyut:** S

### T23: Ayarlar ekranı (token alanı hariç)
- [x] **Kabul:** F2 formu config alanlarını gösterir/düzenler · kaydet doğrular ve atomik yazar · dil anında, notes_dir
  değişince liste yeniden yüklenir · SPEC-tui kabul 6.
- **Doğrulama:** `B && T(tui)` · **Bağımlılık:** T22 · **Dosyalar:** `src/tui/{settings_view.hpp,settings_view.cpp,view.cpp}`,
  `tests/tui/settings_test.cpp` · **Boyut:** M

### ✅ Kontrol noktası 3
- [x] Tüm testler Windows + `F` yeşil
- [ ] Elle: Windows Terminal'de `notes` — Türkçe giriş, edit/micro askı-dönüş, yeniden boyutlandırma
- [ ] Kullanıcı TUI'yi denedi

---

## Faz 4 — Senkron

### T24: 🔴 Temel döngü + kilit + durum
- [x] **Kabul:** commit (ekleme/silme dahil, yazar zinciri) → fetch → güncel/fast-forward → push · SPEC-sync kabul 1 ve 6 ·
  `sync-status.toml` yazılır.
- **Doğrulama:** `B && T(sync)`, `F` · **Bağımlılık:** T03, T14 · **Dosyalar:** `src/sync/{sync_service.hpp,sync_service.cpp,lock.hpp,lock.cpp}`,
  `tests/sync/sync_basic_test.cpp` · **Boyut:** M

### T25: 🔴 Birleştirme + çakışma kopyaları
- [x] **Kabul:** SPEC-sync kabul 2–5 · kodda force push / hard reset yok.
- **Doğrulama:** `B && T(sync)`, `F` · **Bağımlılık:** T24 · **Dosyalar:** `src/sync/sync_service.cpp`,
  `tests/sync/sync_conflict_test.cpp` · **Boyut:** M

### T26: 🔴 `enable` + kimlik doğrulama
- [x] **Kabul:** SPEC-sync kabul 7–8 (5 kurulum durumu, callback tek deneme, token sızıntısı grep testi) · `.gitignore` ·
  HTTPS token: env → keychain · SSH: exec · Secret Service yoksa düz dosyaya düşmez.
- **Doğrulama:** `B && T(sync)`, `F` · **Bağımlılık:** T25 · **Dosyalar:** `src/sync/{auth.hpp,auth.cpp}`, `enable`
  `sync_service.cpp`'de (yardımcıları paylaşır; SPEC-sync karar 8), `tests/sync/{enable_test.cpp,auth_test.cpp}`,
  `tests/sync/fake_ssh_keygen.cpp` · **Boyut:** M

### T27: `AutoSync` + `note sync …` + arka plan senkronu
- [x] **Kabul:** SPEC-sync kabul 9 · `sync`, `status`, `on/off`, `init`, `login/logout` (ekosuz giriş) · hızlı eklemede
  ayrık `note sync --background` (komut ağ beklemez) · SPEC-cli kabul 8.
- **Doğrulama:** `B && T(sync) && T(cli)` · **Bağımlılık:** T26 · **Dosyalar:** `src/sync/{auto_sync.hpp,auto_sync.cpp}`,
  `src/core/process.cpp` (ayrık başlatma), `src/cli/{commands.cpp,sync_commands.cpp}`, `tests/cli/cli_sync_test.cpp`,
  `tests/sync/{auto_sync_test.cpp,recovery_test.cpp}` · **Boyut:** M
- **Not (T27):** SPEC-cli kabul 8'in gerçek kasa kısmı `doctest::skip()` (elle koşuldu, geçti); otomatik testler
  stdin/boş token/host yolunu sınar. Kararlar: SPEC-sync karar 9.
- [x] **T27b:** zaman aşımında süreç ağacını sonlandır (Windows job object, POSIX süreç grubu) · `src/core/process.{hpp,cpp}`,
  `tests/core/process_test.cpp`, `tests/support/fake_editor.cpp`.

### T28: TUI senkron entegrasyonu
- [x] **Kabul:** açılışta/kayıtta/çıkışta `AutoSync` · durum çubuğu (kapalı/✓/⟳/⚠) · Ctrl-S · ayarlarda token alanı
  (maskeli, kasada-kayıtlı göstergesi) ve `enable` akışı, hata satır içinde.
- **Doğrulama:** `B && T(tui)` · **Bağımlılık:** T23, T27 · **Dosyalar:** `src/tui/{main.cpp,view.cpp,settings_view.cpp}`,
  testler · **Boyut:** M

### ✅ Kontrol noktası 4
- [x] Tüm testler Windows + `F` yeşil
- [ ] Elle (kullanıcı reposu ile): HTTPS+token ve SSH ile iki klon arasında senkron; aynı notu iki yerde değiştirince çakışma kopyası
- [x] `security-review`: yol güvenliği, token yaşam döngüsü, senkron — bulgular düzeltildi

---

## Faz 5 — GUI

### T29: GUI iskeleti
- [x] **Kabul:** Notlar/Ayarlar sekmeleri · arama kutusu + fuzzy liste + klasör ağacı filtresi · not görüntüleme ·
  Ctrl+K/↑↓/Enter · 5.000 notla tuş başına ≤ 50 ms · offscreen model testi.
- **Doğrulama:** `B && T(gui)`, elle `notes-gui` · **Bağımlılık:** T16 · **Dosyalar:** `src/gui/{main.cpp,main_window.hpp,main_window.cpp,note_list_model.hpp,note_list_model.cpp}`,
  `tests/gui/note_list_test.cpp` · **Boyut:** M

### T30: Kaydetme akışları
- [x] **Kabul:** SPEC-gui kabul 1–3 · tip kutusu → uzantı değişimi · kapanışta kaydedilmemiş yeni not sorusu.
- **Doğrulama:** `B && T(gui)` · **Bağımlılık:** T29 · **Dosyalar:** `src/gui/{save_dialog.hpp,save_dialog.cpp,main_window.cpp}`,
  `tests/gui/save_test.cpp` · **Boyut:** M

### T31: Klasör işlemleri + içerik araması
- [x] **Kabul:** SPEC-gui kabul 4 · yeni/yeniden adlandır/sil (dolu → sayıyla onay) · Ctrl+M taşı · içerik modunda
  eşleşme sayıları ve editörde vurgu · `QFileSystemWatcher` + yedek tarama.
- **Doğrulama:** `B && T(gui)` · **Bağımlılık:** T30 · **Dosyalar:** `src/gui/{main_window.cpp,folder_actions.cpp}`, testler · **Boyut:** M

### T32: Ayarlar sekmesi + senkron
- [x] **Kabul:** SPEC-gui kabul 6 · token alanı + "Bağlantıyı test et" · durum çubuğunda senkron durumu · F5 · dil değişimi
  yeniden başlatma bildirimi.
- **Doğrulama:** `B && T(gui)` · **Bağımlılık:** T27, T31 · **Dosyalar:** `src/gui/{settings_page.hpp,settings_page.cpp,main_window.cpp}`,
  `tests/gui/settings_test.cpp` · **Boyut:** M

### T33: PDF dışa aktarma (GUI + başsız + CLI)
- [x] **Kabul:** SPEC-gui kabul 5 · Ctrl+E · `note export --pdf` (`notes-gui` yoksa çıkış 3, SPEC-cli kabul 10) ·
  md → `setMarkdown`, diğerleri eş aralıklı düz metin.
- **Doğrulama:** `B && T(gui) && T(cli)`, `F`, elle PDF'te Türkçe · **Bağımlılık:** T32 · **Dosyalar:** `src/gui/{pdf_export.hpp,pdf_export.cpp,main.cpp}`,
  `src/cli/commands.cpp`, `tests/gui/pdf_test.cpp` · **Boyut:** M
- [x] GUI kısmı tamam; CLI `note export --pdf` tamam (`commands_export.cpp`, `tests/cli/cli_export_test.cpp`)

### ✅ Kontrol noktası 5
- [x] Tüm testler Windows + `F` yeşil
- [ ] Elle: Windows'ta GUI — Türkçe giriş, koyu tema, otomatik kayıt, dış değişiklik diyaloğu, PDF
- [ ] Kullanıcı GUI'yi denedi

---

## Faz 6 — Bitiriş

### T34: Kurulum + README
- [x] **Kabul:** `cmake --install` üç ikiliyi kurar · Windows'ta `windeployqt` adımı (kurulum hedefinde) · README: derleme
  (Windows/Fedora), `CMakeUserPresets.json` örneği, PATH kurulumu, kullanım, senkron kurulumu (PAT/SSH).
- **Doğrulama:** temiz prefix'e kurulum + `note --version`, `notes-gui --export-pdf` · **Bağımlılık:** T33 ·
  **Dosyalar:** `CMakeLists.txt`, `src/{cli,tui,gui}/CMakeLists.txt`, `README.md` · **Boyut:** S

### T35: Performans doğrulaması
- [x] **Kabul:** 5.000 üretilmiş notla (Release) SPEC.md §10/2–3 ölçülür ve tutar; tutmayan için profil + düzeltme.
  Modül testlerindeki 5.000 notluk perf vakaları (`search` Debug'da ~12 sn sürüyor) `tests/perf/`'e taşınır; günlük
  `ctest` hızlı kalır, perf yalnız Release'te koşar.
  **Kapandı:** soğuk `list()` 295–917 ms'nin (T16c) nedeni 5.000 dosyanın `binary` koklaması için açılmasıydı.
  `NoteInfo.binary` kaldırıldı; `core::looks_binary` dosyayı zaten okuyan yerlerde uygulanır (TUI önizlemesi,
  `ContentIndex::refresh`, GUI editörü, CLI `show`/`export`). Ölçüm (Release, bu makine, medyan): hızlı ekleme 40–50 ms,
  TUI açılış vekili taze süreçte 60–75 ms (süreç içi 15–23 ms), tuş başına filtre+çizim 8 ms, GUI 6–7 ms, sıcak arama
  28–41 ms. Sınır: dosya sistemi önbelleği testte hep ılık; yeniden başlatma sonrası soğuk açılış elle ölçülmedi.
- **Doğrulama:** `ctest --preset windows-mingw-release -R perf` · **Bağımlılık:** T34 · **Dosyalar:** `tests/perf/` · **Boyut:** S

### T36: Son review
- [x] **Kabul:** tüm dal için `/code-review` + `security-review`; bulgular düzeltildi ya da gerekçesiyle yazıldı ·
  SPEC'ler gerçekleşen kararlarla güncel.
- **Biriken borç (yol boyunca not edildi):** `first_non_empty_line` kuralı `core/note_store.cpp` ve `tui/save_dialog.cpp`'de
  iki kopya → core'dan dışa aç, TUI kullansın · ~~`NoteStore::list()` önbelleği thread-safe değil~~ (T35'te önbellek
  kalktı; TUI'nin `list_mutex`'i artık yalnız store'un yeniden kurulmasını korur).
- [x] Çekirdek + CLI review bulguları: atomik yazımda izin/ACL · `save_config` `[tuning]` · `NoteStore::absolute_path`
  (edit/export) · 8.3 kısa adlar · ayrılmış aygıt adları · terminal kaçış temizliği · süreç sertleştirme ·
  `sync.remote` kimlik bilgisi · `normalize_line_endings` dışa açıldı (POSIX dalları `F`'de doğrulanacak).
- [x] GUI/TUI takibi: `absolute_path` ve `normalize_line_endings` çağrı noktalarına geçiş.
- **Sync review bulguları:**
  - [x] Uzak bağlar düz dosya olarak çıkar (`core.symlinks=false`); bağ olan `.gitignore` okunmaz
  - [x] `NOTEAPP_GIT_TOKEN` yalnız `NOTEAPP_GIT_TOKEN_HOST`'a (varsayılan `github.com`) verilir
  - [x] Merge başladıktan sonraki hata ve yarıda öldürülen merge güvenle geri alınır (kalıcı MERGE durumu yok)
  - [x] Taze klon, klasörde kalmış izlenmeyen `.gitignore`'a takılmaz; satırları birleştirilir
  - [x] `--background` kilit beklemesi `sync_timeout_seconds / 3` ile sınırlı (AutoSync'in öldürmesiyle yarışmaz)
  - [x] Sahibi canlı kilit asla devralınmaz; yaş yalnız sahibi okunamayan kilitte geçerli
  - [x] Çakışma kopyasının adı 255 bayta sığar (kök UTF-8 güvenli kısaltılır)
  - [x] Yazılamayan uzak yol ve harf büyüklüğü çakışması `sync.remote_invalid_paths` ile reddedilir
  - [x] Windows kasası `CRED_PERSIST_LOCAL_MACHINE` (token etki alanında dolaşmaz; eski kayıt okunurken çevrilir)
- [x] GUI/TUI takibi: `absolute_path` ve `normalize_line_endings` çağrı noktalarına geçiş.
- [x] GUI/TUI/uyarı bulguları: PDF görsel yükleme (UNC/NTLM) · ayar kaydında çalışan senkronu öldürme · klasör silme
  onay sayısı · O(n²) CRLF · `absolute_path` (TUI `Enter`, GUI PDF) · config uyarılarının görünürlüğü · `try_write`
  rescan tavanı (ponytail notu).
- **Doğrulama:** tüm testler Windows + `F` · **Bağımlılık:** T35 · **Model:** opus · **Boyut:** M

### ✅ Son kontrol noktası
- SPEC.md §10 başarı kriterleri (2026-10-30, `feat/v1`):
  - [x] 1. Temiz derleme: taze `git clone` + Windows MinGW, boş birimle Fedora 44 / GCC 16; 7/7 paket yeşil (Release'te 8/8).
  - [x] 2. `note "x"`: medyan 34 ms (hedef 200 ms, Release).
  - [x] 3. 5.000 not: açılış vekili 55 ms (500), tuş başına 6,5 ms (50), sıcak içerik araması 26 ms (100). Yeniden başlatma sonrası gerçek soğuk disk ölçülmedi.
  - [x] 4. Tip önerisi korpusta 50/50; TUI ve GUI kaydet diyaloğunda tip değiştirme testli.
  - [ ] 5. Senkron: yerel bare repolarla tüm senaryolar testli; **gerçek GitHub (HTTPS+token, SSH) Windows ↔ Fedora elle testi bekliyor.**
  - [x] 6. Token sızıntısı tarama testi: notes, `.git`, config, log, durum dosyası temiz.
  - [x] 7. TR/EN değişimi testli, katalog anahtar eşliği testi yeşil.
  - [ ] 8. PDF: geçerli dosya ve Türkçe metin çıkarımı doğrulandı (`pdftotext`, T04); **görsel olarak göz kontrolü bekliyor.**
  - [x] 9. GUI ve TUI ayarları aynı `config.toml`'u yazar; `load_config` ile testli.
- [ ] Kullanıcı evde Fedora'da elle son kontrol yaptı

---

## Faz 7 — Qt'yi kaldır, SDL3 + Dear ImGui'ye geç

> Plan: [plan-imgui.md](plan-imgui.md) · Dal: `feat/imgui` · Test eşlemesi: [gui-test-map.md](gui-test-map.md)

- [x] T37: Bağımlılıklar + iskelet. Kanıt: `bash scripts/check-fedora.sh` → exit 0, ctest 8/8 (`e4c50d1`).
- [x] T38: Qt'siz PDF + `note export --pdf`. Kanıt: `bash scripts/check-fedora.sh` → exit 0, ctest 8/8 (`6808c5f`).
- [x] T39: `GuiState`, pencere, liste, editör, kaydetme. Kanıt: temiz birimde `ctest` → exit 0, 8/8 (`f5b1986`).
- [x] T40: Klasörler, içerik araması, tarama. Kanıt: `ctest` → 8/8, `gui_tests` 105 vaka 0 hata (`d8ae8f7`).
- [x] T41: Ayarlar, senkron, PDF diyaloğu. Kanıt: `ctest` → exit 0, 8/8, `gui_tests` 134 vaka 0 hata (`c46abd6`).
- [x] T42: Qt kaldırma + dağıtım (kod). Kanıt: temiz klonda `bash scripts/check-fedora.sh` → exit 0, 8/8 (`5af28e8`).
- [x] T43: Belgeler (`SPEC*.md`, `README.md`). Kanıt: `grep -nE "\bQt|qt6|windeployqt|QT_QPA|offscreen" SPEC*.md README.md` yalnız Değişiklik kayıtlarını ve tarihsel notu bulur.
- [x] T44: Bağımsız review ve test denetimi bulguları. Kanıt: temiz klonda `bash scripts/check-fedora.sh` → exit 0, ctest 8/8 (`c04a202`).
- [x] T45: Linux AppImage. Kanıt: temiz klonda `bash scripts/build-appimage.sh` → exit 0; `bash packaging/linux/smoke-test.sh` fedora:44, ubuntu:24.04, debian:13 → exit 0 (TLS ve `SSL_CERT_FILE=/dev/null` dahil).
- [x] T46: Windows çapraz derleme, MSYS2 yok. Kanıt: temiz klonda `bash scripts/build-windows.sh` → exit 0, yalnız sistem DLL'leri (`708b225`). Wine'da 2 test düşüyor: core `hidden note stays hidden` (Wine sınırı), sync `a push the remote keeps rejecting` (kesin neden yok, 9 koşuda 6 kez; gerçek Windows'ta doğrulanacak).
- [x] T47: Paket belgeleri, betik birleştirme, son derleme. Kanıt: temiz klonda `build-appimage.sh`, `smoke-test.sh` (fedora:44), `build-windows.sh` → exit 0 (`59e38c7`).
- [x] T48: Paket güvenlik review bulguları. Kanıt: temiz klonda `check-fedora` 8/8, `build-appimage`, üç dağıtımda smoke, `build-windows` → exit 0; Wine'da DLL yerleştirme kapısı marker yazmaz (`cedefb4`). `DependentLoadFlags` Wine'da doğrulanamadı.
- [x] T49: Son review bulguları. Kanıt: temiz klonda `build-appimage`, smoke (fedora:44), `build-windows` → exit 0 (`675c671`).
- [x] T50: GUI araç çubuğu ve üst üste binme. Kanıt: `ctest` → exit 0, 8/8; yerleşim testi eski kodda 99 assertion düşer (`bf7da6a`). Gerçek ekran doğrulanmadı.
- [x] T51: TUI liste odaklı klavye. Kanıt: `ctest` → exit 0, 8/8; Release `perf` → exit 0; `notes` tmux e2e 27/27 (`1a4274c`).
- [x] T52: TUI sağ panel önizleme + gömülü editör, `n` ad girişi, alt satır arama, fare ve `<` `>` `z`. Kanıt: `ctest` exit 0 8/8; `tests/tui/e2e_editor.sh` exit 0 42/42 (`cd4f996`).
- [x] T53: GUI review düzeltmeleri (panel boyutu, negatif genişlik, çakışma testi). Kanıt: `ctest` → exit 0, 8/8 (`f81382a`).
- [x] T54: GUI Ayarlar ekranı yeniden düzeni. Kanıt: `ctest` → exit 0, 8/8 (`aaac62b`).
- [x] T57: TUI editör review düzeltmeleri. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, `tests/tui/e2e_editor.sh` exit 0 43/43 (`a38737b`).
- [x] T58: TUI `n` tam pencere taslak, `Ctrl+Q`, `q`, moda göre çerçeve, satır numaraları, format önerisi, sözdizimi renklendirme. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, e2e 85/0 (`87be896`).
- [x] T60: Satır içi öneri, `Ctrl+Space` listesi, `hunspell` yazım denetimi. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, e2e 106/0 (`b8164e6`).
- [x] T61: Sistem panosu, `c`, REVIEW fare seçimi. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, e2e 106/0 (`b8164e6`).
- [x] T55a: Çekirdek: senkronlu `.trash/`, not meta bilgisi, senkron durumu, arama argümanları, birleşik fzf. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0 (`4a67aed`). Windows derlemesi doğrulanmadı.
- [x] T55b: `f`, `d`/`u`/`D`, `T`, açılır meta, `v`, `o`, arama argümanları. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, e2e 106/0 (`b8164e6`).
- [x] G1: Graph çekirdeği. Kanıt: `ctest` exit 0 9/9, ASan/UBSan `graph` exit 0, `build-windows.sh` exit 0, 2-hop 41,8 ms (`71d615f`).
- [x] G2: Birleşim ve oplog. Kanıt: `graph` 104/104, ASan/UBSan exit 0 (`b321704`). Güvenlik düzeltmeleri G2b'de.
- [x] G3: Cypher ve `note query`. Kanıt: `ctest` 9/9, Release `perf` exit 0, üretim sorgusu 20,5 ms (`46dcbb8`).
- [x] G2b: Oplog güvenlik bulguları. Kanıt: `graph` 119/119, ASan/UBSan exit 0, `build-windows.sh --test -R '^graph$'` exit 0 (`1062e8f`).
- [x] G3b: Fuzz. Kanıt: `query` 2,5M ve `decode_frame` 15,1M girdi, ASan/UBSan, 0 artifact; Cypher review bulguları kapalı (`debe7cf`). - [x] G4–G6: Notlar graph'ta, oplog senkronu, geçiş. Kanıt: `ctest` exit 0 10/10; e2e 179/0; smoke üç dağıtım exit 0; fuzz 0 artifact (`64f5ced`, `5d0ee36`).
- [ ] G7: Son doğrulama.
- [x] T55c: T55a review bulguları. Kanıt: `ctest` exit 0 8/8, Release `perf` exit 0, e2e 106/0 (`b8164e6`).
- [x] T62: GUI ad önerisinde gizli değer ve kararsız GUI testleri. Kanıt: `ctest -j8` exit 0 9/9; 48 tam koşuda 0 düşüş (`bb8f293`).
- [x] T64: Çöp kutusu ve tuşların görünürlüğü, GUI çöp görünümü, `NOTEAPP_HOME` yalıtımı. Kanıt: `ctest` exit 0 9/9, e2e 139/0 (`3e5f2b5`).
- [x] T64b: `F2`, `Space`, sağ bilgi, sıra no, sayfa, gezinme hızı. Kanıt: `ctest` exit 0 9/9, e2e 139/0 (`3e5f2b5`).
- [x] T65: Başarısız geri almada öğe korunur. Kanıt: `ctest` 10/10 (`8b8ff7e`).
- [x] T63: Önizleme, öneri tablosu, `hunspell` çıkışı. Kanıt: `ctest` 10/10 (`83e7ed8`).
- [x] T66: `a` liste içi ad, dosya sonuna dokunmama. Kanıt: e2e 179/0 (`7d0a657`).
- [x] T67: C++ kalite turu, RAII, clang-tidy, cppcheck, sızıntı taraması. Kanıt: `ctest` 10/10; Valgrind definite/indirect 0; ASan/LSan 0 (`a1e45fc`). `bugprone-unchecked-optional-access` 140 uyarı kapalı, ~30 örneklendi.
- [x] T68: Metin girişlerinde öneri, `Tab` listesi, renkli arama, geçmiş. Kanıt: `ctest` 10/10, e2e 185/0 (`ed9f9db`).
- [x] T73: Simge tablosu, `icons` ayarı, 30 glifli GUI fontu, lisanslar. Kanıt: `ctest` 10/10, `build-windows.sh` exit 0 (`eb8f727`).
- [x] T69: REVIEW Markdown, CSV, JSON görünümü, `R`. Kanıt: `ctest` 10/10, e2e 194/0 (`497e7af`).
- [x] T70: TUI klasör gezinmesi, klasör içi numara, klasör işlemleri. Kanıt: `ctest` 10/10, e2e 218/0, Release `nav perf` exit 0 (`e6271d3`).
- [x] T71: Çoklu seçim, `y`/`m`/`p`, `g` atlamaları. Kanıt: `ctest` 10/10, e2e 250/0 (`23d884a`).
- [x] T74: Ağaç liste, `t` taşı, dolu ok, Nerd simgeler. Kanıt: `ctest` 10/10, e2e 250/0 (`23d884a`).
- [x] T75: TUI hızı: değişen hücre çıktısı (tuş başına 3.318 → 228 bayt), tek kare, `install-local.sh`. Kanıt: `ctest` 12/12, pty bayt testi (`abb085f`).
- [~] T76: Yazi görünümü geri alındı (kullanıcı kararı, 2026-11-02); hız işi T75 kalıyor.
- [x] T77: G7 bulguları, atlanan dosya kaydı, perf testi, Windows DLL kontrolü, statik analiz. Kanıt: `ctest` 12/12, e2e 258/0, tidy `src` 0 (`db435b9`).
- [x] T86: Klasör kopyası, `install-local.sh`, atlanan dosya temizleme. Kanıt: `ctest` 12/12 (`03ee16b`).
- [x] T87: Geçişte okunamayan dizin raporu, `.trash`, README, sanitizer kurulumu. Kanıt: `ctest` 12/12, Wine `note_graph` exit 0, ASan `cli`+`note_graph` exit 0 (`9ac5cc9`).
- [ ] T78: Bağlantılar paneli: gelen, giden `[[bağlantı]]`, `RELATED_TO`, etiketler; TUI ve GUI. Depends: `main`.
- [ ] T79: TUI komut paleti (`:`). Depends: T78.
- [ ] T80: Etiket: `#etiket` metinden, `tag:` arama argümanı. Depends: T79.
- [ ] T81: Kayıtlı Cypher görünümleri. Depends: T80.
- [ ] T82: GUI'yi TUI ile eşitle: ağaç liste, sağ bilgi, sıra no, Markdown önizleme. Depends: T81.
- [ ] T83: TUI `main` fonksiyonunu böl (CCN 79). Depends: T82.
- [ ] T59: LSP istemcisi, varsayılan kapalı. Depends: graph.
- [~] T56: LadybugDB denemesi iptal: kullanıcı dış bağımlılık yerine kendi küçük graph depomuzu istedi (2026-11-01).
- [ ] T84: GUI'de grafik not haritası. Depends: T59.
- [ ] T85: Sekmeler. Depends: T84.
- [ ] Elle (kullanıcı): Fedora KDE'de gerçek pencere, Türkçe giriş, koyu tema, PDF göz kontrolü.
- [ ] Elle (kullanıcı): Windows 11'de çapraz derlenmiş `.exe`, Türkçe IME, koyu tema, pano.
