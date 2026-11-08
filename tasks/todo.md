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
- [x] G7: Son doğrulama (her main push'unda Linux/Wine/AppImage).
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
- [x] T78: Bağlantılar paneli, `L` ilişki, `b` odak; GUI paneli. Kanıt: `ctest` 12/12, e2e 262/0 (`01e334a`).
- [x] T88: Geçişten sonra `notes_dir`'e bırakılan dosyalar: `status` raporu, `migrate --new`; silinen not geri gelmez (`migrated-digests`). Kanıt: `ctest` 12/12 exit 0, RED `cli` exit 8 (`17e96a5`); review bulgusu kapandı.
- [x] T79: TUI komut paleti (`:`). Kanıt: `ctest` 12/12 exit 0, e2e 276/0, RED `command_palette` (`b9886b8`); review: bulgu yok. Açık şüphe: `input_view.cpp:81-94` genişliği ham metinle ölçüyor, bidi'li adda ipucu kayabilir (kozmetik, T83'te test).
- [x] T89: GUI tek gezgin: klasör+not tek ağaç, Ağaç/Ayrıntı görünüm seçici, tüm eylemler üst araç çubuğunda. Kanıt: `check-fedora` 12/12 exit 0, `gui_tests` 246/246, RED `browser_test` SIGABRT (`1dfa805`); review sürüyor.
- [x] T80: Etiket: `#etiket` metinden, `tag:` arama argümanı; GUI arama argümanları; bağlantı paneli senkron düzeltmesi. Kanıt: birleşik `check-fedora` 12/12 exit 0, e2e 276/0, GUI ASan/UBSan exit 0 (`166c264`); iki review bulguları kapandı.
- [x] T81: Kayıtlı Cypher görünümleri (salt okunur), `view:` argümanı, TUI paleti. Kanıt: `ctest` 12/12, e2e 276/0, RED 6 test; security review F1 (önbelleksiz hata, donma) `21ac35f` ile kapandı.
- [x] T82: GUI'yi TUI ile eşitle: Bilgi paneli, sıra no, Markdown önizleme. Kanıt: konteyner `ctest` 12/12, RED 4 test dosyası, ekran görüntüleri (`b34223a`); review bulguları (uzun satır donması, başlık gizlenmesi) `caea804` ile kapandı, birleşik `check-fedora` 12/12 exit 0.
- [~] T90: emekli → P serisi (kullanıcı 2026-11-03: TUI ve GUI birebir aynı özellikler, ikisi de ortak çekirdeğin ince arayüzü).
- [x] P: Arayüz eşitliği — plan [plan-parity.md](plan-parity.md). Envanter: 98 özellik, 53 TUI-ağırlıklı / 34 GUI-ağırlıklı fark, 16 yinelenen mantık.
  - [x] A0: `noteapp_app` + `tests/app`; same_listing, TokenStore/config_warnings/sync_restart, kCopySuffix, GUI in_folder/join_folder taşındı (`900b23a`, app/core/tui/gui 4/4). D4 `split` kararı: var olan yollar için core `split_note_path` public, iki arayüz onu kullanır (R görevinde).
  - [x] A8: `noteapp_edit` (`src/edit`): editör tamponu, oturum, sözdizimi, bul, yazım, tamamlama, pano, alt süreç, LSP; UI include'u yapılandırmada reddedilir; ad alanı `tui::` kaldı (`d823774`; edit/tui/lsp_client 3/3, Windows `note`+`notes` exit 0). Açık: MinGW release'te `notes-gui` derlenmiyor (`view_dialogs.cpp:141` stringop-overflow, G0'dan) → G2+G3'e verildi.
  - [x] A2: `app::Listing` ağaç/Ayrıntı/arama tek kural, tercihe göre sıralama, bozuk argümanda son geçerli sorgu (`45556fe`, app 20/20).
  - [x] A1: `app::kCommandRegistry` (84 komut), TUI ve GUI tabloları kimliğe bağlı, eşitlik bekçisi testi + sahipli eksik listesi (TUI 11, GUI 53) (`1d4e767`, app/tui/gui 3/3). Son komut sınırı `limits.app.recent_commands` → L2.
  - [x] A3: `app::TrashOps` iki arayüzde; GUI klasör silme çöpe (geri alınabilir), GUI geri alma yığın, `limits.app.trash_undo_steps` (`5c89d14`, app/tui/gui 3/3). Listede kalıcı silme (`D`, `Shift+Del`) onaylı olarak kalır — kullanıcının önceki isteği ('shift+d kalıcı'). Review: 1 orta (TUI kalıcı klasör silme eski listeyle sayıp canlı tüm notları siler) → A3b bitti: kalıcı klasör silme yalnız gösterilen notları siler, görünmeyen not varsa klasör kalır; geri alma adımları yol + atılma anı; ayrıca `limit_keys()` dizi boyutu 38→39 derleme kırığı düzeltildi (`5aff17b`).
  - [x] A5: `app::Document` iki arayüzde; TUI otomatik kayıt, GUI bayt koruma (CRLF/BOM), ortak salt-okunur kuralı, yeni not klasör kuralı (`81f95c0`; app/tui/gui/lsp_client 4/4). TUI: vurgulu alt klasörde `n`/`a` notu o klasöre koyar (eşitlik). Review: 1 orta (klasör yeniden adlandırınca arka plan sekmesinin belgesi eski yolda) → A5b `df795a9` (RED + gui geçti). Birleşik kontrol 7/7.
  - [x] A4, A6, A7: ortak modüller (commands, listing, trash, move/copy/link, document, settings/sync/prefs, preview). Depends: A0.
  - [x] R3: GUI `app::Listing` üstünde; Sırala menüsü, genel arama, etiket/görünüm seçici, `sync:`, kalıcı açık klasörler; ağaç tercihe göre sıralı (`1c5b284`, gui+app geçti). Bilinen: yalnız adı eşleşen satırda boş ikinci satır; açık klasörler yalnız kapanışta yazılır; popup Esc doğrulanmadı.
  - [x] R1: TUI `app::Listing` üstünde; `limits.app.recent_commands` (`2c3cf73`, core/app/tui 3/3). İçerik modunda listede olmayan sonuç artık düşer (GUI ile aynı, kabul). Açık: Listing'in son geçerli sorgusu klasör/çöp görünümünde sıfırlanmıyor.
  - [x] R2/R4: not işlemleri, belge, ayarlar ortak modüllerden (A3–A7 ile birlikte).
  - [x] G0: `view.cpp` (3072) ve `gui_state.cpp` (2478) 19 panel dosyasına bölündü, davranış aynı (`664875e`, gui geçti, satır kümesi karşılaştırması).
  - [x] G2+G3: GUI çoklu seçim (çöpte de), yank/yapıştır (`app::item_ops`), hızlı ekleme, sütunlar (senkron/kaynak), klasör taşı/kopyala, çöp önizleme, komut paleti, F1 yardım, ilişki, gezinme, bölme, simge aç/kapa; R3 artıkları; MinGW `notes-gui` derlemesi düzeldi (`e824d65`; app+gui geçti, Windows exit 0). GUI'de kalan: `external`, `copy` (G4), `find*` (G5). Review: engelleyici yok; öneri: işaret sayısı durum çubuğunda, `delete_marked` gösterilen notu açıkça bıraksın, `icons.toggle` + `[lsp]/[limits]` round-trip testi (G4'e).
  - [x] G4: GUI `external` (Shift+E), `copy` (Ctrl+C), CSV/JSON önizleme (ayrıştırma `src/app`), arama geçmişi/Tab tamamlama/gölge/argüman renkleri (`InputLine`), başıboş dosya bildirimi, işaret sayısı (`4921473`; app 60/60, edit 148/148, tui 682/682, gui 389/389). Arama modu kısayolu Tab → Ctrl+Shift+M (TUI ile eşit, kabul).
  - [x] G5a–c: GUI editör widget'ı, sözdizimi/yazım/tamamlama, LSP. Depends: A8, G0.
  - [x] T1+T2: TUI `save`, `links.toggle`, `view.tree/details`, `folder.new`, `export.pdf`, bağlantı testi, `[[wiki]]` takibi, sağ tık menüsü; `app::item_ops` (klasörü üstüne yapıştırma hatası) (`91b2034`; 6 grup + e2e 284/0). Security review: Sev-1/2 yok; Sev-3: test token'ı iptalde kasada kalır, test notları push eder (GUI ile aynı), remote değişince token yeni host'a gider, PDF kırık symlink uyarısı; `enable` UI iş parçacığında ağa çıkar.
  - [x] IMP: dosyayı not olarak içe aktar (kullanıcı 2026-11-03), CLI `note import`, TUI `notes import` + palet komutu, GUI "Dosya içe aktar…" + sürükle-bırak; ortak `app::import_files`. Faz 1 bitti: `app::import_paths`, `note import PATH... [--folder|--type|--name|--overwrite|--json]` (`77230db`; app+core+cli geçti). Faz 2 bitti (`a536b18`): `notes import`, `notes --help`/`notes-gui --help`, TUI palet `import`, GUI araç çubuğu menüsü + sürükle-bırak; review engelleyici yok. main `68d477a` (Linux 16/16, Wine 14/14, AppImage). Açık (düşük): belleğe sığmayan dosyada `bad_alloc` yakalanmıyor (doğrulanmadı); büyük klasör içe aktarma UI iş parçacığında, ilerleme yok; gerçek masaüstünde diyalog/sürükle-bırak elle denenmedi. Not: `validate_new_note` tipte `/` kabul ediyor (CLI kendisi reddediyor) → temizlik işine.
  - [x] CLI-C: `note completion zsh|bash|fish` + gizli `note __complete` (not, klasör, tür, etiket, görünüm, sorgu argümanı, config anahtarı), eksiksiz `note help KOMUT` (kullanıcı 2026-11-04). Bitti (`dbc7bfc`; cli/completion_shell/core 3/3; zsh'te gerçek Tab denendi; install-local.sh kuruyor). `completion` ayrılmış sözcük (kabul). `notes --help` / `notes-gui --help` yok → IMP faz 2'ye.
  - [x] UI-C: GUI ayarlar `default_type` açılır listesi içeriği tam görünmüyor (kullanıcı 2026-11-04). `combo_field` (`src/gui/view_widgets.cpp:308`) NoPreview popup'ı ok genişliğinde/kısa; liste alan genişliğinde ve tüm öğeler okunur olmalı (aynı widget yeni not formunda da var).
  - [x] MAP-R: haritada fareyle ilişki kurma (kullanıcı 2026-11-04). Shift+sol sürükle not→not: ilişkili (RELATED_TO) ya da [[wiki]] bağlantısı seçimi; not↔etiket: etiket ekle; kenara sağ tık: ilişkiyi/etiketi kaldır. Mantık `src/app`'te, GUI fare + TUI harita (fare + klavye) eşit.
  - [x] UI: arayüz yenileme + boşta çizim yok, plan `tasks/plan-ui.md` (onaylı 2026-11-05). Sıra: R ve C paralel → U1 (önce/sonra PNG kullanıcıya) → U2. R bitti (boşta GUI 0 kare, CPU %0,4–0,5; TUI boşta 0 çizim), C bitti (T1–T12 + review düzeltmeleri), U1 bitti (`theme.{hpp,cpp}`, WCAG AA testli, TUI yuvarlak çerçeve, palet kenar hatası); hepsi review'lı, feat/graph-ui'de. U2 bitti (kullanıcı onayı 2026-11-05): GUI G3–G14 + Inter/JetBrains Mono (`gui-state.toml`), tek satır araç çubuğu, ⚙/Ctrl+, Ayarlar, öncelikli durum çubuğu, fontlar kopyasız (RSS 52,6→43,5 MB); TUI T3+T5 (kip hapı, F1 sabit ipucu çubuğu, palet satırı). Review'lı; Linux 16/16, Wine 14/14, AppImage. Ertelenen: TUI panel iç boşluğu (fare eşleme + ~5 test), Ayarlar'dan çıkışta kaydedilmemiş soru (önceden de yok).
  - [x] Sonraki sürüm (başladı 2026-11-03, main 8b759b7 sonrası): G5a–c bitti: GUI editörü `EditorBuffer` üstünde (satır no, durum çubuğu, bul, IME, sanallaştırma, sözdizimi, yazım, tamamlama, LSP) (`a75526c`; gui/edit/app/lsp_client 4/4; Release 10 MiB tuş başına en kötü 23 ms). Açık: GUI taslağında tip yok → yazım/renk ilk kayda kadar yok. Review: engelleyici yok (ImGui olay sırası hipotezi doğrulanmadı), T3+T4 bitti: TUI sekmeler (`app::SavedTabs`, `Ctrl+T/W`, `Alt+N/P`, `Ctrl+K`), TUI harita (braille, `app::MapModel`), `limits.app.tabs`, `limits.app.map_nodes`; eşitlik listesi iki tarafta da boş (`77c39de`; app/edit/tui/gui 4/4, e2e 297/0), ardından L1+L6+A4/A6/A7+Sev-3 temizlik. Temizlik bitti (`8b42cd6`, feat/graph-ui'de): L6 (`limits.graph.import_line_values`, kalan sabitler gerekçeli), `validate_new_note` tipte ayırıcı reddi, bağlantı testi işçide + token kasaya yazılmaz + host değişince silinir, PDF kırık bağ sorusu, `app::DraftTypeTracker` (GUI taslak tipi), TUI listeye tık sekmeyi kapatmaz (`EditorSession::park`), `limits.app.*` testleri; RED→GREEN, e2e 297/0; cpp-reviewer + security-reviewer engelleyici yok. Açık (düşük): TUI breadcrumb/çöp tıklaması odağı listeye almıyor; kasa anahtarı port'u içermiyor (önceden var); GUI `save_token` boş token; `kMaxExtensionBytes` iki yerde; `commands_import.cpp` yorum eskidi; GUI test sürerken Kaydet `sync.lock` bekleyişi; `LspConfig::kMax*` kullanıcı kararı. Eski liste: G5a–c (GUI editörü: satır no, bul, renklendirme, yazım, LSP), T3 (TUI sekmeler), T4 (TUI harita), A4/A6/A7, L1, L6, yukarıdaki Sev-3'ler.
- [x] T83: TUI `main` fonksiyonunu böl. Kanıt: lizard CCN 78 → en çok 15, birleşik `check-fedora` 12/12 exit 0, e2e 276/0 (`108facc`); review: davranış farkı yok. Açık (düşük): `input_view.cpp:81` `tail_cells` ham genişlikle keser, ESC'li adda satır taşar.
- [x] T59: LSP istemcisi (T59a–f bitti; kullanıcı kararıyla varsayılan açık). Plan: [plan-lsp.md](plan-lsp.md) (onaylı: diagnostics+completion, Windows, geçici dizin).
  - [x] T59a: JSON core'a + RFC 8259, LSP framing/protokol. Kanıt: `ctest` 12/12, Windows derleme exit 0, `fuzz_lsp` 8.1M girdi 0 çökme, RED 22+59 assertion (`b5f6b83`); security review: fuzz kapsamı (yanlış yeşil, 182→6682 sayaç) ve O(n²) tampon `21ee5be` ile kapandı; 5 dk fuzz 17.2M girdi 0 artifact.
  - [x] T59d: Windows pipe'lı `ChildProcess` (overlapped pipe, job object). Kanıt: Linux `ctest` 12/12, Wine `ctest` 11/11, Wine 10/10 süreç testi, RED Wine 9 hata (`25a6217`); review: bulgu yok, `spawn_detached` bayrakları korunmuş. Elle: gerçek Windows'ta 10 test.
  - [x] T59b: `LspClient` + sahte LSP sunucusu. Kanıt: birleşik Linux `ctest` 13/13, Wine `ctest` 12/12 (`cc74a75`); security review 2 Sev-2 + 1 Sev-3 `ca63ac4` ile düzeltildi (`lsp_client` 20/20 x3, ASan temiz), yeniden doğrulama: kapandı; 2 Sev-3 (kapanış süresi, UTF-16 O(n·m)) `6642085` ile düzeltildi, `lsp_client` 20/20 tekrar, ASan temiz. Kapanış bulgusu için RED kurulamadı (mikrosaniyelik yarış penceresi).
  - [x] T59c: config, çalışma alanı yansıtma, TUI tanı + completion. Kanıt: `ctest` 13/13, e2e 276/0, ASan 3/3, Wine 12/12, tuş başına en kötü 7,37 ms (Release, 2 MiB) (`308f1b8`); security review 5 madde + varsayılan açık LSP (python, c/cpp, sh, md, lua, html, css, dockerfile, sql) ve dil tablosu `1927496` ile: `ctest` 13/13, e2e 276/0, ASan 3/3; Wine 11/12 (perf `folder copy` 201 ms / sınır 200 ms, LSP dışı). İkinci security review: Sev-2 inline `lsp = {…}` kayıtta `sync.lsp` oluyor, Sev-3 göreli PATH (yalnız LSP araması için düzeltilecek) → T59e'ye verildi.
  - [x] T59e: Riskli sunucular sertleştirilerek varsayılan açık (rust-analyzer, gopls, jdtls, ts, yaml, json, taplo, htmx-lsp), dil başına çoklu sunucu, ayar dosyası yansıtma yasağı (rust-analyzer.toml, node_modules/…). Kanıt: `ctest` 13/13, ASan 3/3, e2e 276/0, Wine 11/12 (perf `folder copy` 222 ms → T93) (`74d09b9`); security review: 2 Sev-2 → T59f.
  - [x] T59f: Unicode katlama + dosya sistemi eşdeğerlik kontrolü, uzak dosya referanslı not sunucuya gitmez, sunucu cwd `state_dir/lsp` + Windows `NoDefaultCurrentDirectoryInExePath`, rust-analyzer `checkOnSave` nesne biçimi, ilk eşitleme kapısı. Kanıt: core/tui/lsp_client/tui_pty 4/4, ASan 3/3, e2e 284/0, Wine lsp_client+gui geçti; Wine perf 10 MiB editör zamanlamaları ~50 ms sınırda (yük altında, T93) (`611abbf`). Security re-review: 1,3,4,5 kapandı; uzak referans tespiti atlatılabiliyor (F1 biçimler, F2 önbellek, F3 kapsam) → T59g.
  - [x] T59g: yeniden açılan not tampon kuşağıyla yeniden taranıyor; Windows'ta genel ağ yolu kuralı (ham + JSON/yüzde çözülmüş), per-key dedektör kaldırıldı; README düzeltildi. Kanıt: RED 16 atlatma + yeniden açma, `lsp_client` 68/68, `tui` 807/807, Wine 6/6 (`a3b3f01`).
- [x] NX (kullanıcı 2026-11-05 "kalan işlere devam et"): main f34be62
  - [x] NX-A TUI: panel içi 1 sütun boşluk (fare eşleme + testler), breadcrumb/çöp tıklaması odağı listeye alır, üst satır sağ grubu 60 sütunda senkron hatasını kesmez, editör bildirimi F1'i itmez.
  - [x] NX-B GUI+TUI: Ayarlar'dan kaydetmeden çıkınca sor (⚙, Ctrl+,, ←, Esc; iki arayüz); L1 kalanı: önizleme 256 KiB tavanı ve harita derinliği config (`0 = sınırsız`).
  - [x] NX-D önizleme + küçükler: biçimli önizleme yerleşimi parça parça (20 MiB'ta 926 ms ilk kare / 90 ms kare → `limits.tui.view_bytes` varsayılanı kalkabilsin); TUI ham REVIEW'da uzun satırda yatay kaydırma (GUI ile eşit); palet komutu çıkış sorusu Vazgeç'te "son kullanılanlar"a yazılmasın; `settings_page.cpp:123` yorumu; TuiSession Ctrl+C bağlantı, token-sonra-enable-hatası, Ayarlar'da düzenleme tuşu ve kaydırılmış ListClipper testleri.
  - [x] NX-C core/app: içe aktarmada `bad_alloc` öğe hatası olur; büyük içe aktarma UI iş parçacığı dışında, ilerleme + iptal (iki arayüz); `kMaxExtensionBytes` tek sahip; GUI `save_token` boş token eşitliği; T93 perf testi kararlılığı.
  - [x] NX-F: önizleme referans taraması çok satırlı ve `\]` kaçışlı etiketleri bulsun; etiket katlama Unicode (md4c ile aynı, Türkçe İ/ı dahil); liste imi sonrası TAB çit sütunu; özet çakışmasında O(k²) önle; Wine'da yük altında düşen perf testleri (`editor_latency_test.cpp:204` en kötü örnek, `graph_perf_test.cpp:297` tek örnek) medyan + Wine çarpanı.
- [x] NY (kullanıcı 2026-11-05):
  - [x] NY-G GUI: not seçilince sağ panel önizleme (REVIEW) açılır, düzenleme düğme/kısayolla (TUI gibi); haritada düğüm seçilince diğerlerinin titremesi; harita ilişki kurma keşfedilebilir olsun (düğüm sağ tık "Bağla…", ipucu); arayüz öğeleri büyür; işlem bildirimleri (toast) iki arayüzde, ortak `app` modeli.
  - [x] NY-T TUI: ilk açılışta sol liste sağdan bariz dar; dışarıdan yapıştırmada satır arası fazladan boş satır ve sonda çok sayıda boş satır (CRLF/bracketed paste); TUI haritada titreme varsa.
  - [x] SAN: sanitizer derlemesi GCC 16/UBSan'da `src/tui/commands.hpp:52` ve `src/gui/commands.hpp:184` (consteval/static_assert içinde adres–nullptr karşılaştırması) yüzünden kırık; `app::has_command` ile düzelt, `scripts/check-sanitize.sh` yeşil.
  - [x] NY-R çizge çalışma ortamı, plan [plan-graph-map.md](plan-graph-map.md) (onaylı): F1 adlı ilişki, F2 keşif, F3 sorgu çubuğu (yazma dahil, A7 mutasyon kontrolü: iki koruma da testlerce yakalanıyor), F4 özellik denetçisi (cpp + security review, Sev-3 GUI clipper ve seçim kayması düzeltildi) BİTTİ. Eski not: adlı ilişkiler (kullanıcı 2026-11-05): ilişki kurarken kenara ad verilir; daha önce kullanılan adlar seçim listesi olarak gelir (yeni ad da yazılabilir). GUI harita (sürükle/sağ tık Bağla), TUI (`L`/harita `C`), CLI karşılığı; ad haritada kenar etiketi ve Bağlantılar panelinde görünür; senkronla taşınır; aramaya girer (NY-S). Önce cpp-architect tasarımı (çizge şeması/oplog/birleştirme etkisi), şema değişikliği gerekiyorsa kullanıcıya sorulur.
  - [x] NY-T3 (düşük): yama başlığındaki "without another copy" yalnız ayrıştırıcı→kuyruk için doğru (FTXUI OnEvent değerle kopyalar) — metni daralt; birim test: limit sınırında ve timeout sınırında bölünen ESC[201~; SPEC'te kayıp bitiş işaretinde `paste_timeout` boyunca tuşların metne girdiği; sürüklenen TUI liste genişliği `tui-state.toml`a kalıcı yazılır (kullanıcı 2026-11-05: evet).
  - [x] NY-P önizleme: L1 (b) JSON/CSV ve tek dev blok parçalı → `limits.tui.view_bytes` varsayılanı 0; markdown önizleme doğruluğu (CommonMark/GFM örnekleriyle md4c HTML'e karşı fark listesi, GUI+TUI).
  - [x] NY-S arama: CLI/TUI/GUI aramasında çizge ilişkileri de (etiket, kenar, ilişkili/bağlı notlar, kenar özellikleri); sonuçta eşleşme nedeni; ortak `app::Listing`/core.
  - [x] NY-T2 TUI tuşları yeni CLI bayraklarına uyar: n adla ekle, a düz ekle, s ara (f gizli eş), Ctrl+S senkron, S göster, E düzenle, d çöpe, m taşı, x dışa aktar (seçim v, sütun V), T etiketler (çöp t), i içe al.
  - [x] NY-C CLI kısa komutlar (kullanıcı kararı 2026-11-05): `-n`=ad ve `-f`=klasör kalır; `-a METİN` düz ekleme (TUI `a`), `-n AD METİN` adla ekleme (TUI `n`); `-l` ls, `-s` find, `-S` show, `-E` edit (`-e` editör seçeneği zaten var → TUI `E` gibi), `-d` rm, `-m` mv, `-x` export, `-T` tags, `-i` import; uzun eşler `--ls`, `--find`…; `note cat` = show; eski yazımlar çalışır; tamamlama + yardım + SPEC-cli.
- [x] PERF-ED (2026-11-06, F4'ten önce de var): Linux Release `editor_latency_test.cpp:62` 10 MiB `aa` ghost+spell kapısı yük altında koşuların ~1/6–2/6'sında düşüyor (taban eaade5b ve HEAD aynı: medyan tuş ~35,5 ms, tipik en kötü ~41 ms, tepe 50–71 ms; bütçe 50 ms). Kapı gevşetilmez; gölge öneri/yazım denetiminin tuş başı maliyeti profillenip düşürülecek. Kanıt: A/B 6+6 koşu, scratchpad `runs/`. BİTTİ: kök neden tuş başı tüm satırı yeniden sarma ve sütun sayımı; yalnız düzenlenen kuyruk yeniden hesaplanıyor (`0816487`, `b9f1ffd`): medyan 33,3→0,77 ms, en kötü 37,2→10,3 ms; Release perf 2×61/61.
- [x] L: Sabit sınır yok — plan [plan-limits.md](plan-limits.md) (kullanıcı isteği 2026-11-03).
  - [x] L0: `[limits]`, `core::Limit`, birimler, `0 = sınırsız`; içerik sonuç sınırı tek anahtar (`0582e9a`). Bilinen: ayar ekranı kaydı dosyada sonradan değişen limit/tuning değerini açılıştakine döndürür.
  - [x] L1: GUI (sekme, harita, önizleme). BİTTİ: JSON/CSV ve tek dev blok NY-P'de parçalandı, `limits.tui.view_bytes` varsayılanı 0. Sekme/harita düğüm/derinlik config'te, markdown önizleme parçalı. Kalan: JSON/CSV ve tek dev blok parçalı değil → `limits.tui.view_bytes` varsayılanı 256 KiB kalıyor (karar: markdown'a ayrı sınır mı, önce JSON/CSV parçalama mı).
  - [x] L2+L5: `limits.tui.*` (görünüm, renklendirme, tamamlama, geçmiş, yapıştırma, geri alma…), `limits.lsp.*` + LSP süreleri; JSON derinlik sınırı kalktı (döngüsel yazıcı); `limit_keys()` artık `std::to_array`; kMaxNameTries, folder_path_bytes, GEÇİCİ köprüler ve Listing son sorgu düzeltildi (`5bca2cd`; tui/lsp_client/core/app/search/cli 6/6). Açık: görünümler tembel üretilmiyor (sınırsız yapılırsa büyük notta yavaş); 256 KiB `[` JSON süresi yeniden ölçülmeli (eski 45 ms / bütçe 50).
  - [x] L3: not başına bağlantı/etiket sınırı yok (bütçeli devam), hedef/etiket uzunluk, klasör derinliği ve 1000 kopya sınırı kalktı; `limits.core.folder_path_bytes`, `limits.search.*`, `limits.sync.merge_input_bytes`; dosya adı sınırı tek sahip (`0001d89`; core/note_graph/search/sync 4/4). Kalan: GUI/app'te eski imza köprüleri (GEÇİCİ) ve TUI `kMaxNameTries` → L1/L2; A48 `FolderNode::recent` ölü kod (silme onayı).
  - [x] L4: graph/sync/sorgu sınırları config (`limits.graph/query/view.*`), görünümler korumalı profil, satır içi import bölme; birleşimi belirleyen 3 sabit protokolde kaldı (`f4db1a3`, graph/core/note_graph/cli 4/4). Security review: Sev-2 çerçeve u32 tavanı > SQLite 1e9 → L4b'ye eklendi; Sev-3 sahte cihaz günlükleri `peers` yuvalarını doldurup senkronu durdurabilir → L4c.
  - [x] L4c: nedensel cihaz kabulü; sahte `.aside-*` dosyaları gerçek kopyayı sildirmiyor (`e9e4b86`; graph 206/206, note_graph 104/104, core 223/223, sync 109/109). Kalan: önceden kabul edilmiş sahte cihaz yeri kalıcı. Doğrulama: 2 Sev-2 (reddedilen çerçeve yine 'gerekli' işaretliyor; `referenced_` sınırsız) → L4d bitti: referans yalnız kabul edilen sıradaki çerçeveden, gerekli küme `peers × deps` ile sınırlı ve sayaçlı, TOOBIG'de rebuild bayrağı geri, `partial_tail` temizliği, kenara alma yolları göreli (`6821c78`; graph 211/211, sync 109/109). Son güvenlik doğrulaması L serisi sonunda tek turda.
- [ ] Ertelendi (kullanıcı 2026-11-08: "şimdi değil"; depo özel, yalnız kullanıcının cihazları yazıyor — kullanmaya başlayınca ya da depo paylaşılınca yeniden bakılır): senkron günlükleri imzasız; depoya yazabilen biri cihaz kabulünü ve günlükleri etkileyebilir. Cihaz başına imza (ed25519) ADR adayı. Eski: `admit` cihazları ilk-gelen sırasıyla değil nedensel ihtiyaca göre (bilinen cihazların bağımlılıkları önce) kabul etsin; sahte cihaz yuvaları doldurup gerçek cihazları dışarıda bırakamasın. Hipotez: sahte `*.oplog.aside-*` dosyaları gerçek kenara ayrılmış kopyaları sildirebilir. Depends: L4b.
  - [x] L4b: oplog v2 — mantıksal çerçeve dışa aktarımda parçalara bölünür, grup tamamlanınca uygulanır; yerel işlem çerçeve sınırıyla reddedilmez; eski sürüm `log_too_new` ile durur (veri kaybı yok); SQLite bağlama denetimleri; import satırı atomik (`d987fc6`; graph 200/200, note_graph 104/104, core 223/223, cli 172/172, sync 109/109). Security review: Sev-2 saldırganın parça sayısıyla işlem tavanı (6 GB çözüm, OOM), Sev-2 SQLite sınırına yakın grup tüm ingest'i düşürür, Sev-3 yarım grup her senkronda yeniden okunur → L4b2 bitti: `group_ops` (4Mi, çözülmüş en kötü ~2,5 GiB), TOOBIG/oom yalnız o günlüğü durdurur, yarım grup bütçeye sayılır ve değişmedikçe yeniden okunmaz, `fuzz_log` (`298f23f`). — kullanıcı onayladı 2026-11-03 (eski sürüm cihaz senkronu durdurup güncelleme ister, veri kaybı yok). Depends: L0, L4.
  - [x] L6: çıplak sayı taraması; `limit_keys()` elle boyutlanan diziyi `std::to_array`/boyutsuz tabloya çevir (birleşmelerde iki kez kırıldı). Depends: L1–L5.
- [x] Lead kararı (2026-11-08, tam yetkiyle): atlansın — fix/leftovers'ta uygulanıyor. `find_program` göreli PATH girdilerini (`.`, `node_modules/.bin`) editör/git/pano için de atlasın mı? Şimdilik yalnız LSP araması atlıyor.
  Kanıt: f554d94 (find_program yalnız mutlak PATH; Windows çocuk süreçlerinde NoDefaultCurrentDirectoryInExePath), aea2003 (pager kabuksuz, mutlak yol), ae74f29 (pager ve ssh-keygen without_secrets); Linux 17/17, Wine 15/15; security review.
- [x] T91: Editör notun kendi bidi karakterlerini (U+202E vb.) terminale ham basıyor (Trojan Source tarzı yanıltma, senkronla gelen notta). T59c sırasında görüldü. Kanıt: `ctest` 13/13, e2e 280/0 (`693cc89`); ek iş: ZWJ/ZWNJ listeden çıkar, CLI tty çıktısını temizle (`bbdee5b`, `ctest` 13/13, e2e 284/0). Security review: F1-F3 TUI sink'lerinde `terminal_safe` eksik (çıkış notu, editör bilgi çubuğu/modal/prompt, durum satırı), liste genişletme (Hangul doldurucular, U+2061-2064, U+180E) → T91b sırada.
- [x] T91b: T91 review düzeltmeleri: çıkış notu, editör çubukları/modal/prompt, durum satırı sink'lerinde `terminal_safe`; liste 24 öğe, ön filtre listeden derleme zamanında. Kanıt: `ctest` 13/13, e2e 284/0, RED her sink için (`a80764d`).
- [x] Bilinen (düşük): Windows mintty/MSYS terminali TTY sayılmıyor, `note show` ham basıyor (`commands.cpp:75`).
  Kanıt: 2ea7221 (MSYS/Cygwin pty adı TTY sayılır), afad654 (eko kapanamıyorsa token istenmez, uyarı); gerçek mintty'de elle deneme kullanıcıda.
- [ ] Elle (kullanıcı): gerçek dil sunucusuyla (pyright/clangd) LSP denemesi.
- [~] T56: LadybugDB denemesi iptal: kullanıcı dış bağımlılık yerine kendi küçük graph depomuzu istedi (2026-11-01).
- [x] T84: GUI'de not haritası (Harita görünümü, komşuluk/tüm çizge/etiketler). Kanıt: Debug `ctest` 13/13, `gui_tests` 298/298, Release 2.000 düğüm/4.999 kenar kare 17–20 ms, RED 10+14 doğrulama (`0dc47c5`); review: bulgu yok; refactor diff'leri (`09a6ab8`, `449aa90`) lead kontrolüyle saf çıkarma.
- [x] T92: Hedeften önce yazılan `[[bağlantı]]` hedef oluşunca `LINKS_TO` alıyor (oluşturma/kopya/taşıma/geri alma/senkron). Kanıt: `ctest` 13/13, RED 8 test, Release ölçüm (`f9510e4`); review: bulgu yok, 2 düşük hipotez → T92b.
- [x] T92b: kullanıcı işlemleri op tavanıyla reddedilmiyor (link_appeared, delete_node, remove/purge/empty_trash, klasör silme/kopya, import, view silme, çakışma çözümü partili); `[tuning] graph_batch_ops`; çakışma kazananı/kopyası `update_links`. Kanıt: `ctest` 13/13, RED 9 test, `note_graph_tests` 96/96 (`46070f8`); Release perf yük altında oynak (temiz ölçüm T93'te). Kalan 4 'ikisi de' sınır → plan-limits lead kararları; review: 1 orta bulgu (`delete_node` sınırda klasör kenarını erken siler) → T92c.
- [x] T92c: kısmi silme klasör kenarlarını son geçişe bırakıyor; remove/purge NodeId ile devam; H2 SPEC'e not. Kanıt: RED 2 test/12 assertion, `note_graph_tests` 98/98, `core_tests` 213/213 (`0913b15`); review: 1 orta (klasör düğümü bütün silmede tavan; çöpteki notlar IN_FOLDER tutuyor) → T92d.
- [x] T92d: klasöre gelen kenarlar da partili; yalnız düğümün kendi klasör kenarı son geçişe kalır. Kanıt: RED (20 çöp notlu klasör, bütçe 8), `note_graph_tests` 99/99, `core_tests` 213/213 (`275d792`). T92e: klasör döngüsü `again`'de durur (`4068d5c`, RED + 100/100, 213/213).
- [x] T93: Test kararlılığı: harita perf testini `tests/perf`'e (RUN_SERIAL) taşı; Wine'da `folder copy perf` 201 ms / sınır 200 ms nedeni.
- [x] T85: GUI editör sekmeleri. Kanıt: `ctest` 13/13, `gui_tests` 314/314, RED 6/6 (`089e928`); review düzeltmeleri `f5d70b8` (`gui_tests` 320/320, RED 3), yeniden review: bulgu yok. Kullanıcıya bildir: taslaktan başka nota geçince kayıt sorusu yok (taslak sekmesinde kalır), düzenlenen sekme 'korunur'.
- [ ] Elle (kullanıcı): Fedora KDE'de gerçek pencere, Türkçe giriş, koyu tema, PDF göz kontrolü.
- [ ] Elle (kullanıcı): Windows 11'de çapraz derlenmiş `.exe`, Türkçe IME, koyu tema, pano.

## SD — Sadeleştirme (denetim 2026-11-06, kullanıcı: "tablodaki tüm maddeleri yap")
Kural: davranış birebir korunur (madde 1/4/7 hariç: geriye uyum yok kararıyla eski veri yolu kalkar). Testler değiştirilmez; yalnız silinen özelliğin testleri silinir. Her adım: worktree, cpp-reviewer, Debug ctest yeşil, sonra `feat/graph-ui`'ye merge. İki şerit paralel (en çok 2 derleme).
- [x] SD1 (şerit A): legacy migration alt sistemini sil + uyum artıkları (madde 1+7). Scope: `src/core/migration*`, `src/cli/commands_graph.cpp`, `src/app/stray_notice*`, `src/tui/main.cpp`, `src/gui/*migration*`, `src/core/note_snapshot.cpp`, `src/cli/main.cpp`, `src/sync/sync_service.cpp`, `src/sync/auth.cpp`, `src/tui/view.cpp`, `src/gui/gui_state.cpp`, config/i18n, ilgili testler ve SPEC. `export_all_notes` korunur.
  Kanıt: `ctest -j2` (SD1 worktree) → 16/17, düşen yalnız kararsız `lsp_client` diagnostics flood (tek başına 5/5); cpp-reviewer engelsiz; merge 129656d, 78 dosya +531/−3433. Ek: `import_notes/import_folders/relink_notes` üretimden kalktı → `tests/support/note_import.hpp`.
- [x] SD-F kararsız testler: GUI tooltip (`main_window_test.cpp:821`, -j2'de 13==10) ve `lsp_client` diagnostics flood (`language_servers_test.cpp:432`, 2==1) yük altında deterministik olsun; kontrol gevşetilmez.
  Kanıt: tooltip kod hatasıydı (c0cfd86, yük altında 20/20); LSP flood test yarışıydı (b8cf681, yük altında 10/20 → 20/20, mutasyon 3/3 düşer).
- [x] SD2 (şerit A, Depends: SD1): oplog v1 biçimini kaldır, hep v2 (madde 4) + `[limits.query]`/`[limits.view]` tek profile (madde 8). Scope: `src/graph/oplog*`, `src/graph/merge.cpp`, `src/core/config*`, `res/defaults.toml`, ilgili testler/SPEC.
  Kanıt: ctest -j2 → 16/17 (yalnız bilinen tooltip, SD-F); cpp-reviewer engelsiz; cea4dc6 net −72. Madde 8 (profil birleştirme) YAPILMADI: `[limits.view]` senkronla gelen görünümleri kullanıcı sorgusu sınırsızken de sınırlı tutar (`note_views_test.cpp:298`, `cli_query_test.cpp:152`) — bilinçli güvenlik ayrımı.
- [x] SD3 (şerit B): küçük kopyalar (`pop_code_point`, `sync_state_text`, `https_host`, 26 i18n çifti → `app.*`) + ayar formu `app::SettingsForm` (madde 6+5). Scope: `src/tui/settings.cpp`, `src/tui/save_dialog.cpp`, `src/gui/settings_page.cpp`, `src/gui/note_editor.cpp` (yalnız pop_code_point), `src/app/settings*`, `src/core/text*`, i18n.
  Kanıt: SD3 dalı ctest 17/17; cpp + security review (Sev-3 terminal_safe düzeltildi); merge fe268ac, birleşik ctest 17/17. Madde 8 dışı karar: TUI is_printable (bayt) / is_typable (UTF-8) iki kural kaldı.
- [x] SD4 (şerit B, Depends: SD3): editör mod mantığı (find/complete/ghost) `src/edit/` altında ortak; GUI `NoteEditor` ve TUI `EditorSession` onu kullanır (madde 2). Scope: `src/edit/editor_session*`, `src/edit/*modes*`, `src/gui/note_editor*`.
  Kanıt: ctest 17/17, Release editör gecikmesi değişmedi (69,34→69,30 ms toplam), cpp-reviewer engelsiz; merge 2adaaa7, net −104.
- [x] SD5a (Sev-2 güvenlik, SD5 ölçümünden): `pdf::parse_markdown` her bloğa `Block::nesting` kopyalıyor → iç içe listede bellek O(n²) (64 KiB girdi → 508 MB RSS; GUI önizleme ve PDF dışa aktarma etkileniyor). Temsil düzeltmesi (tavan yok), çıktı birebir aynı, deterministik doğrusallık testi.
  Kanıt: 64 KiB 507→19 MB RSS, 128 KiB 1981→27 MB; GUI alıntı çubuğu kutuları da doğrusal; 12 girdide eski kodla piksel eşit (a55980d); ctest 17/17; cpp-reviewer engelsiz; merge 3ffef5a.
- [x] SD5 (şerit B, Depends: SD4) — ERTELENDİ (2026-11-07): ölçüm net ~130 satır, ortak model TUI'nin 8 bilgisini taşımıyor ve SD5a'daki O(n²) hatasını TUI'ye taşırdı. SD5a'dan sonra 8 eksik bilgiyle yeniden ölçülür.: TUI markdown çizimi ortak `pdf::parse_markdown` blok modeline (madde 3); `markdown_view_test` ve `markdown_conformance_test` değişmeden geçer. Scope: `src/tui/markdown_view*`, ortak blok modeli taşınacaksa `src/app/markdown_blocks*`, `src/pdf/markdown*`, `src/gui/markdown_preview.cpp` (yalnız include).
  Kanıt (dar kapsam, lead kararı): md4c bayrakları/entity çözme/hizalama/liste imi tek yerde (`src/pdf/md4c_parse.*`, 33fb08c), TUI görsel alt metni BR farkı düzeltildi (500fded); ctest 17/17 ×2; cpp-reviewer engelsiz. Çizim arayüze özgü kaldı.
- [x] Düşük (önceden var): md4c handler'ında exception (ör. bad_alloc) C çerçevelerinden geçiyor; md4c `-fexceptions`'sız derleniyorsa std::terminate. md4c derleme bayrağını kontrol et; gerekirse handler'da yakala ve ayrıştırmayı durdur.
  Kanıt: SD11a `29f70f4` (merge `11c6945`): md4c C olarak `-fexceptions`sız derleniyor; her geri çağırma yakalar, sıfır dışı döner (md4c durur, durumunu bırakır), `parse_md4c` `md_parse` dönünce yeniden fırlatır (`src/pdf/md4c_parse.hpp`). Test: `tests/pdf/markdown_blocks_test.cpp` "md4c: a handler exception stops the parse and reaches the caller after md4c returns".
- Kullanıcı 2026-11-06: "tek uygulama, üç arayüz" — CLI/TUI/GUI'de mantık tekrarı yok, mantık `src/app`/çekirdekte; arayüz yalnız girdi eşleme + çizim. Tarama: ~600 satır tekrar, 15 bulgu. Kopyalar farklıysa tek davranış seçilir (kural: veri güvenli olan > bilgi veren > katı olan); terminal/pencereye özgü olan (zoom sınırı, zaman genişliği, dosya üzerine yazma onayının etkileşimi) parametre olarak arayüzde kalır.
- [x] SD7 WP-A (şerit B, Depends: SD3): `app/ui_prefs` (TUI elle ayrıştırıcı + GUI toml++ → tek toml++; ListColumn tek tablo), `app::quick_add` (boşluk kırpılır; var olan klasör → ileti + seç), `app::confirm_text` (TUI'nin çoğullu + alt klasör sayılı metni), `app::note_fields`/`column_text` (GUI'nin type+tags alanlarıyla), `app::search_enter_action` (boş eşleşmesiz sorguda Enter not AÇMAZ — GUI bug), `app::clipboard_text`, unlink TUI de `app::unrelate_map_notes` (relation_not_found), `command_block`/`prune_marks`/`trash_targets` tek kopya (TUI'nin gizli denetimiyle). Scope: `src/tui/{list_actions,tui_state,commands,view}*`, `src/gui/{gui_state,gui_state_marks,gui_state_folders,gui_state_trash,gui_state_commands,view_browser}*`, `src/app/{ui_prefs,item_ops,trash_ops,commands,listing}*`, i18n.
  Kanıt: ctest 17/17; cpp + security review; 2 gerçek veri kaybı düzeldi (açık klasörler silinmesi; onay açıkken senkron sonrası yanlış hedefin kalıcı silinmesi — HIGH, 6524b17, bağımsız doğrulandı); içerik araması süzgeç indekste (10k `a type:txt` 632→82 ms Debug); bozuk durum dosyası `.bad`'e; merge f1d33cf. Kalan düşük: revizyon kontrolü ile remove ayrı işlem (TOCTOU), çöp yolu revizyon kontrolsüz → SD11b.
- [x] SD8 WP-B (şerit A, Depends: SD1): `app::SyncIndicator` (açılışta `status()` okunur; hata bildirimi Error'a GEÇİŞTE, tekrar etmez), `core` yerel saat biçimi dışa açılır ve üç arayüz onu kullanır, `app::MapCamera::fit/zoom_at` (sınırlar arayüz parametresi), GUI denetçi etiketleri `app::Inspector::add/remove_label` (kirli tamponu önce kaydet + `tag_key` normalizasyonu app'e taşınır, TUI de kazanır). Scope: `src/tui/{sync_indicator,map_view,main}*` (main yalnız 750-770), `src/gui/{gui_state_sync,view_map,gui_state_inspector}*`, `src/app/{sync_indicator,graph_layout,map_camera,inspector}*`, `src/core/text*`, `src/tui/tui_state.cpp` yalnız zoom satırları.
  Kanıt: ctest 17/17, cpp-reviewer engelsiz (kilit bırakma güvenli); TUI kirli sekmede etiket yazma hatası düzeldi; merge bbfa8c1, birleşik ctest 17/17.
- [x] SD9 WP-C (şerit A, Depends: SD2): CLI `ls`/`search` `app::Listing` hattını kullanır (ilişki eşleşmesi CLI'da da — NY-S eşitliği), `note query` okuma sonuçları `core::run_user_query` ile not yoluna çözülür, hücre metni `app/map_query` ile tek; CLI yeni not tip kuralı `app/import_files` ile tek. Scope: `src/cli/{commands,commands_query}*`, `src/app/{listing,map_query,import_files}*`.
  Kanıt: ctest 17/17, cpp-reviewer (orta: find limit süzgeçten önce — d241172 düzeltildi); merge 2d2bcd6. Aynı hata TUI/GUI'de → SD7 madde 12.
- [x] SD10 WP-D (şerit A, Depends: SD4, SD8, SD9): `app::DocumentSession` (save/autosave/çakışma/on_listing; GUI ve TUI aynı; Gone → farklı kaydet), `app::TabList` (keeps: boş olmayan metin korunur — veri güvenli), `app::export_note_pdf` + `default_export_name` (`split_note_path().name`; "dosya var" sonucu döner, etkileşim arayüzde), `app::move_note`. Scope: `src/gui/{gui_state_save,gui_state_tabs}*`, `src/tui/{editor_tabs,save_dialog,list_actions(yalnız export)}*`, `src/edit/editor_session*`, `src/app/{document,tabs,export}*`, `src/cli/commands_export.cpp`.
  Kanıt: ctest 17/17, Release editör gecikmesi gerilemedi, cpp-reviewer engelsiz; merge a7fa71c. DocumentSession → app::Document'a eklendi; TabList<T> yapılmadı (→ SD11). CLI export artık --force olmadan ezmez.
- [x] SD11 eşitlik açıkları (Depends: SD7, SD10): + SD5'ten: TUI önizleme `[[wiki]]` bağlantılarını ayrıştırmıyor (düz metin), GUI ayrıştırıyor — TUI de bağlantı olarak göstersin; + SD10'dan kalanlar: `app::TabList` (TUI editor_tabs ve GUI gui_state_tabs sekme listesi), GUI `gui_state.cpp` `[[tabs]]` yazımı `app::format_saved_tabs`'e, GUI `close_tab`/`confirm_close` yalnız-boşluk taslağı sormadan atıyor (TUI soruyor → veri güvenli: sor), iptal edilen Gone isteminden sonra not geri gelirse autosave kalıcı kapalı kalıyor mu (`editor_session.cpp:262`, test ile doğrula, kapalıysa düzelt); GUI klasör özeti (boyut, son güncelleme, son notlar); kayıtlı görünüm kaydet/sil TUI ve GUI'de (`app` üzerinden); bağlantı hedefi iki arayüzde `resolve_link`. (GUI'de klasör kalıcı silme yok = önceki karar, dokunulmaz.)
  Kanıt: SD11a (wiki önizleme TUI, Gone sonrası autosave TUI+GUI, GUI boşluk taslağı, md4c exception) merge 11c6945; SD11b (app::TabList, GUI klasör özeti, TUI/GUI görünüm kaydet/sil, L→resolve_link, revizyon denetimli remove/trash TUI+GUI+CLI, görünüm silme onay kimliği) merge 65d66f8; ctest 17/17; cpp + security review engelsiz.
- [x] SD6: son doğrulama (Debug, Release perf, Wine, AppImage) → `main` squash + push.
  Kanıt (2026-11-07): Linux Debug 17/17; Release perf 61/61 ×2; AppImage build exit 0 + `--help` 0; Wine ilk koşu 13/15 (iki yeni testte Linux varsayımı: uzun Windows temp yolu satırdan taşıyordu, fare 'belirme'si hareket sayılmıyordu) → test düzeltmesi 06f352d, Wine 15/15.
- [x] TUI ayarlar hata satırı tek satır (56 hücre): uzun config yolunda 'nothing was saved' ve sözdizimi hatasının satır/sütunu görünmüyor (Linux'ta da). Satır sarılsın ya da yol kısaltılsın.
  Kanıt: b9fdb6f (kutu genişliğinde sarılır, ileti ve satır/sütun her zaman görünür).

- [x] TUI harita oturumu (kullanıcı 2026-11-08: "K ile kaydedip yeniden açınca map view gelmiyor"): harita kipi, etkin görünüm, derinlik ve süzgeçler `[map]` olarak TUI ve GUI'de saklanır; açılışta görünüm ayrı depo bağlantısında, salt-okunur, iptal edilebilir çalışır. Kanıt: merge d646c9d (cf79beb, 37fd702, 9cd3f21), pty uçtan uca test eski kodda düştü, ctest 17/17, iki review.
- [ ] İzle (2026-11-08): Wine'da `gui_tests` bir kez çöktü (`main_window_test.cpp:92` CHECK, GuiState+0x1fb8 üstünde ImDrawVert verisi → yığın bozulması); 26+ tekrarda ve Linux ASan/UBSan'da üretilemedi. Bulunan tek hata (libgit2 global arama yolu yarışı, TSan 8 uyarı) 8aac41b ile düzeltildi ama çökmeyle ilişkisi kanıtlanmadı. Sonraki doğrulamalarda Wine gui'yi tekrarlı koş; yeniden görülürse Windows'ta Application Verifier / page heap ile incele.
