# Spec: noteapp — çapraz platform not uygulaması

> Durum: **ONAYLANDI** · Tarih: 2026-10-28
> Bu dosya yetenek haritası + tüm modüllerin ortak kurallarıdır. Her modülün ayrıntısı
> `SPEC-<modül-id>.md` dosyasındadır; hangi spec'lerin var olduğunun tek dizini aşağıdaki haritadır.

## 1. Amaç

Terminalde yaşayan biri için hızlı, sunucusuz, sahibine ait bir not sistemi:

- `note "metin"` ile saniyesinde not ekle; ya da `note` deyip sevdiğin editörde (edit/micro/nano/vim…) yaz.
- `notes` ile TUI aç: solda fzf hissiyle notlar arası gezin, sağda içeriği gör.
- `notes-gui` ile aynı notları masaüstü penceresinde oluştur/düzenle/gez; Ayarlar sekmesinden yapılandır.
- İstenirse (aç/kapa) notlar private bir GitHub reposuyla senkron kalsın; Windows ↔ Fedora arasında.

**Kullanıcı:** tek kişi (geliştirici), iki cihaz: Windows 11 (iş) ve Fedora Linux (ev).

**Tasarım ilkeleri (araştırmadan: nb, Joplin, Trilium, QOwnNotes, zk, nvALT):**
- Notlar **gömülü graph veritabanında** (`graph.db`, [SPEC-graph.md](SPEC-graph.md), 2026-11-01). Kimlik kalıcı düğüm
  kimliğidir; yol, ad ve klasör kenarlarından türetilir; tip = uzantı. Baytlar aynen durur. Düz dosya yalnız dışa aktarma
  biçimidir: `note export --all` notları uygulamadan bağımsız geri verir.
- Arama kutusu aynı zamanda oluşturma kutusudur (nvALT): yazdıkça filtrele, eşleşme yoksa Enter yeni not.
- Kaydetmede veri kaybı yok: atomik yazma, dışarıdan değişiklik tespiti, senkron çakışmasında iki kopya.
- Ayarlar az ve tek dosyada; CLI/TUI/GUI aynı dosyayı paylaşır; sırlar asla bu dosyada değil.

## 2. Yetenek haritası (onaylandı: 2026-10-28)

| Modül id | Sorumluluk | Bağımlı olduğu | Spec |
|---|---|---|---|
| `core` | Not deposu (not ve klasör graph düğümü, tip=uzantı), yol güvenliği, atomik yazma, config, i18n (tr/en), editör başlatma, platform dizinleri | — | [SPEC-core.md](SPEC-core.md) |
| `detect` | İçerikten tip önerisi (puanlı sezgisel) | — | [SPEC-detect.md](SPEC-detect.md) |
| `search` | fzf tarzı ad/yol eşleştirme + tam metin içerik arama | core | [SPEC-search.md](SPEC-search.md) |
| `sync` | Oplog senkronu, libgit2 yalnız taşıyıcı; aç/kapa; HTTPS+token ve SSH; çakışmada iki kopya | core | [SPEC-sync.md](SPEC-sync.md) |
| `pdf` | `noteapp_pdf` kütüphanesi: not metnini A4 PDF'e dizer (libharu + md4c, gömülü DejaVu). Ayrı spec dosyası yok; davranış [SPEC-cli.md](SPEC-cli.md) ve [SPEC-gui.md](SPEC-gui.md) "PDF dışa aktarma" bölümlerinde | core | — |
| `cli` | `note` komutu: hızlı ekleme, ls/find/show/edit/rm/mv/export/sync/config | core, detect, search, sync, pdf | [SPEC-cli.md](SPEC-cli.md) |
| `tui` | `notes`: fzf tarzı liste + önizleme, kaydet diyaloğu, ayar ekranı | core, detect, search, sync | [SPEC-tui.md](SPEC-tui.md) |
| `gui` | `notes-gui`: klasör ağacı + liste + editör, Ayarlar sekmesi, PDF dışa aktarma (SDL3 + Dear ImGui) | core, detect, search, sync, pdf | [SPEC-gui.md](SPEC-gui.md) |

**Yapım sırası:** `core` ∥ `detect` → `search` ∥ `sync` ∥ `pdf` → `cli` → `tui` → `gui`
(`cli`/`tui`/`gui` birbirinden bağımsız; sıra değer teslimine göre.)

Bağımlılık yönü tek yönlü; döngü yok. `note export --pdf` `notes-gui`'yi **çağırmaz**: `note` ve `notes-gui`
`pdf` modülüne doğrudan link olur. PDF bu yüzden GUI'siz derlemede (`-DNOTEAPP_BUILD_GUI=OFF`) çalışır.

## 3. Teknoloji

| Bileşen | Seçim | Sürüm (pin) | Lisans | Nereden |
|---|---|---|---|---|
| Dil | C++23 (`std::expected`, `std::filesystem`) | — | — | — |
| Derleme | CMake + Ninja, `CMakePresets.json` | CMake ≥ 3.28 | — | sistem / CLion |
| Derleyici | Windows: Linux'ta çapraz derleme, Fedora `mingw64-gcc-c++` 16.2.1 (`docker/windows-cross.Containerfile`); yerelde PATH'teki MinGW-w64 `g++` (doğrulanmadı). Fedora: GCC ≥ 14, macOS: Apple Clang | — | GPL + runtime exception (GCC) | konteyner imajı / sistem |
| TUI | FTXUI | v7.0.3 | MIT | FetchContent |
| GUI arayüzü | Dear ImGui (`imgui_impl_sdlrenderer3`) | v1.93.0 | MIT | FetchContent |
| GUI pencere/girdi/çizim | SDL3 (statik; SDL_Renderer, yazılımsal çizime düşebilir) | release-3.4.18 | zlib | FetchContent |
| PDF | libharu + md4c (Markdown ayrıştırma) + kendi yerleşim kodumuz (`src/pdf`) | libharu v2.4.6, md4c v0.6.0 | zlib, MIT | FetchContent |
| Font (GUI + PDF) | DejaVu Sans / Sans Mono, ikiliye gömülü (`resources/fonts`) | sürüm depoda kayıtlı değil | Bitstream Vera (serbest) | depoda |
| Config / i18n | toml++ (`TOML_EXCEPTIONS=0`) | v3.4.0 | MIT | FetchContent |
| Git senkron | libgit2 (`USE_SSH=exec`, HTTPS: WinHTTP / OpenSSL / SecureTransport) | v1.9.7 | GPLv2 + linking exception | FetchContent |
| Sır saklama | Windows: Credential Manager API (`CRED_PERSIST_LOCAL_MACHINE`); Linux/macOS: hrantzsch/keychain (libsecret / Keychain) | v1.3.1 | MIT | sistem / FetchContent |
| Test | doctest | v2.5.3 | MIT | FetchContent |
| Fuzzy algoritma | fzy algoritması (jhawthorn/fzy) — C++'ta yeniden yazım | ref. v1.1 | MIT | — |

- Windows'ta **tüm proje** MinGW-w64 GCC ile derlenir (Linux'ta çapraz: preset `windows-cross-*`, toolchain
  `cmake/toolchains/mingw-w64-x86_64.cmake`; yerelde: `windows-mingw-*`). İkililer statiktir ve `msvcrt.dll`'e bağlanır;
  `scripts/build-windows.sh` yalnız Windows sistem DLL'lerine izin verir. `std::print` kodda kullanılmaz (eski GCC 13
  sınırından kalan kural; kaldırılabileceği doğrulanmadı).
- MSVC kullanılmaz; gerekirse ileride preset eklenir. macOS yalnız Mac'te derlenir; çapraz derleme yok.
- Makineye özel yollar preset'lere yazılmaz: yerel derlemede araçlar PATH'ten gelir.
- Fedora sistem paketleri (`docker/fedora.Containerfile`): `gcc-c++ cmake ninja-build git libsecret-devel openssl-devel
  pkgconf-pkg-config poppler-utils` (poppler-utils = PDF testlerinin `pdftotext`'i) ve SDL3 için `libX11-devel libXext-devel
  libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel dbus-devel ibus-devel
  libxkbcommon-devel mesa-libGL-devel mesa-libEGL-devel mesa-libGLES-devel wayland-devel wayland-protocols-devel libdecor-devel`.
  GUI'siz derleme (`-DNOTEAPP_BUILD_GUI=OFF`) için SDL3 paketleri gerekmeyeceği beklenir; ayrı doğrulanmadı.
- Listede olmayan her yeni bağımlılık **önce sorulur** (§9).

## 4. Komutlar

```bash
# Windows ikilileri Linux'ta (podman ya da docker) — çıktı out/windows-x86_64/{note,notes,notes-gui}.exe
bash scripts/build-windows.sh
bash scripts/build-windows.sh --test   # önce Wine'da test; şu an sıfırdan farklı çıkar (core "hidden note stays hidden" =
                                       # Wine sınırı; sync "a push the remote keeps rejecting" aralıklı, neden çözülmedi)
```

```bash
# Windows'ta yerel derleme — doğrulanmadı; PATH'te MinGW-w64 g++, Ninja; test için pdftotext.exe
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug --output-on-failure
ctest --preset windows-mingw-debug -R "core"          # tek modül testleri
cmake --build --preset windows-mingw-release
```

```bash
# Linux AppImage (podman ya da docker) — çıktı out/noteapp-<sürüm>-x86_64.AppImage
bash scripts/build-appimage.sh
```

```bash
# Release varlıkları — önce build-appimage.sh ve build-windows.sh; çıktı out/release/<sürüm>/ (yükleme elle)
bash scripts/make-release.sh 0.1.2
```

**Release sözleşmesi** (kendi kendini güncelleme buna dayanır: `src/app/updater.cpp` denetler,
`src/app/update_install.cpp` kurar): tag `vX.Y.Z`; varlıklar tam olarak `noteapp-X.Y.Z-x86_64.AppImage`,
`noteapp-X.Y.Z-windows-x86_64.zip` (kökte yalnız `note.exe`, `notes.exe`, `notes-gui.exe`, sahiplik işareti
`noteapp-package.txt` ve `licenses/`; bağ yok) ve `SHA256SUMS` (`sha256sum` çıktısı, iki varlığın birer satırı); AppImage
`note X.Y.Z` bildirmeli. AppImage `usr/share/noteapp/install-local.sh` ve yanında `noteapp.desktop` taşır: yerel kurulumun
güncellemesi bu betiği `--keep 2 --expect-version X.Y.Z --progress-dir <dizin>` ile çalıştırır. Güven modeli (TLS +
aynı release'in SHA256SUMS'u, imza yok): `.engineering/adr/0001-update-trust-model.md`.

```bash
# Fedora doğrulaması konteynerde (docker, çalışmıyorsa rootless podman)
scripts/check-fedora.sh            # fedora:latest imajında derle + ctest (GUI testleri SDL_VIDEO_DRIVER=dummy)
```

```bash
# Fedora (paket listesi: §3)
sudo dnf install gcc-c++ cmake ninja-build git libsecret-devel openssl-devel pkgconf-pkg-config   # + SDL3 başlıkları, §3
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug --output-on-failure
cmake --build --preset linux-gcc-release && cmake --install build/linux-gcc-release --prefix ~/.local
```

```bash
# Biçimlendirme (her iki platform)
clang-format -i $(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp')
```

Çalıştırma: `note`, `notes`, `notes-gui` (install sonrası PATH'te). Test/geliştirmede config ve veri
dizini `NOTEAPP_HOME` ortam değişkeniyle geçici bir dizine yönlendirilebilir.

## 5. Proje yapısı

```
CMakeLists.txt            → kök; alt dizinleri ekler, seçenekler: NOTEAPP_BUILD_GUI (ON; kapalıyken SDL3/ImGui indirilmez), NOTEAPP_BUILD_TESTS (ON)
CMakePresets.json         → windows-mingw-{debug,release}, windows-cross-{debug,release}, linux-gcc-{debug,release}, macos-clang-{debug,release}
cmake/toolchains/mingw-w64-x86_64.cmake → Linux'ta Windows çapraz derleme toolchain'i (Wine ile test)
scripts/check-fedora.sh   → docker (yoksa podman) ile Fedora derleme + test (kontrol noktası doğrulaması)
scripts/build-windows.sh  → konteynerde Windows .exe üretir (--test: Wine'da test)
scripts/build-appimage.sh → konteynerde Linux AppImage üretir
scripts/make-release.sh   → out/'taki AppImage ve Windows ikililerinden release varlıklarını ve SHA256SUMS'u üretir
scripts/container-engine.sh → çalışan motoru (docker, yoksa podman) seçer; build-windows.sh kullanır
docker/fedora.Containerfile → Fedora derleme imajı (bağımlılıklar önbellekli)
docker/windows-cross.Containerfile → Windows çapraz derleme + Wine imajı
resources/fonts/          → gömülü DejaVu fontları + lisans (GUI ve PDF)
cmake/Dependencies.cmake  → tüm FetchContent pin'leri tek yerde
cmake/EmbedResources.cmake→ resources/* dosyalarını derleme anında ikiliye gömer
resources/i18n/en.toml    → İngilizce mesaj kataloğu (kaynak dil)
resources/i18n/tr.toml    → Türkçe mesaj kataloğu
resources/defaults.toml   → varsayılan config (tip listesi, varsayılan tip, süreler…)
src/core/                 → noteapp_core (statik kütüphane)
src/detect/               → noteapp_detect
src/search/               → noteapp_search
src/sync/                 → noteapp_sync
src/pdf/                  → noteapp_pdf (libharu + md4c; GUI'siz derlemede de var)
src/cli/                  → `note` çalıştırılabilir
src/tui/                  → `notes` çalıştırılabilir
src/gui/                  → `notes-gui` çalıştırılabilir (SDL3 + Dear ImGui)
tests/<modül>/            → doctest testleri, modül başına bir test çalıştırılabilir
tests/detect/samples/     → tip algılama örnek korpusu (<tip>/*.txt)
SPEC.md, SPEC-*.md        → bu spec'ler
tasks/plan.md, tasks/todo.md → plan ve görev listesi
```

## 6. Kod stili

- **İsimlendirme:** tipler `PascalCase`, fonksiyon/değişken `snake_case`, üye `trailing_`, sabit `kPascalCase`,
  namespace `noteapp::<modül>`. Kodda Türkçe isim **yok**; yorumlar, mesajlar ve dokümantasyon Türkçe.
- **Hatalar:** sınırda (dosya sistemi, ağ, kullanıcı girdisi) `std::expected<T, Error>`; `Error` makine-okunur
  `code` + i18n anahtarı + argümanlar taşır. İstisna kontrol akışı için kullanılmaz; toml++ istisnasız.
  Yakalanıp yutulan hata yok — ya kullanıcıya gösterilir ya loglanır.
- **Doğrulanmış değerler:** sınırda bir kez doğrula, sonra tipe güven (`NotePath::parse`).
- **Metin:** her yerde UTF-8 (`std::string`, kaynak dosyalar UTF-8); Windows konsolunda `SetConsoleCP/OutputCP(CP_UTF8)`,
  argümanlar `wmain`/`GetCommandLineW` ile UTF-8'e çevrilir.
- **Kullanıcıya görünen her metin** i18n kataloğundan (`tr("key", {args})`); kodda gömülü UI metni yok.
- **Biçim:** `.clang-format` (LLVM tabanlı, 4 boşluk, 110 sütun). Uyarılar: `/W4` / `-Wall -Wextra -Wpedantic`, hata olarak.

```cpp
// src/core/note_store.hpp
namespace noteapp::core {

// notes_dir'e göre göreli, doğrulanmış yol: mutlak yol, "..", ayrılmış ad ve geçersiz karakter içeremez.
class NotePath {
public:
    static std::expected<NotePath, Error> parse(std::string_view relative);
    const std::filesystem::path& value() const { return value_; }

private:
    explicit NotePath(std::filesystem::path value) : value_(std::move(value)) {}
    std::filesystem::path value_;
};

class NoteStore {
public:
    explicit NoteStore(std::filesystem::path root);

    std::expected<NotePath, Error> create(const NewNote& note);
    // Dosya expected_mtime'dan sonra dışarıda değiştiyse üzerine yazmaz: Error::Code::ModifiedExternally.
    std::expected<void, Error> write(const NotePath& path, std::string_view content,
                                     std::filesystem::file_time_type expected_mtime);

private:
    std::filesystem::path root_;
};

}  // namespace noteapp::core
```

## 7. Test stratejisi

- **Çerçeve:** doctest; her modülün `tests/<modül>/` altında kendi test çalıştırılabiliri; `ctest` hepsini koşar.
- **Seviyeler:**
  - *Small (~%80):* saf mantık — yol doğrulama, slug, UTF-8 katlama, fuzzy puanlama, tip algılama, config ayrıştırma.
  - *Medium (~%15):* geçici dizinde gerçek dosya sistemi (NoteStore), **yerel bare git reposu** ile senkron
    (ağ yok; `file://` remote), `note` ikilisini alt süreç olarak çalıştıran CLI testleri, FTXUI ekranını
    metne render eden TUI testleri, `SDL_VIDEO_DRIVER=dummy` + yazılımsal çizimle başsız GUI model/diyalog testleri (Xvfb gerekmez), `pdftotext` ile PDF geri okuma.
  - *Large (~%5, elle):* gerçek GitHub reposuyla Windows ↔ Fedora senkronu, gerçek editörlerle akış,
    GUI elle kontrol listesi (her modül spec'indeki "Elle doğrulama" bölümü).
- **Kritik yollar** (veri kaybı/güvenlik): yol kaçışı, atomik yazma, dışarıdan değişiklik, senkron çakışması,
  token sızıntısı — bunlar için önce kırmızı test (TDD), her edge-case ayrı test.
- **Kapsam:** `core`, `detect`, `search`, `sync` mantığında hedef ≥ %80 satır kapsamı (Fedora'da gcov ile ölçülür;
  zorunlu kapı değil, düşüşte uyarı). Ön yüzlerde kapsam hedefi yok; davranış testleri + elle liste.
- Testler ağa çıkmaz; GitHub'a gerçek erişim yalnız elle (Large) testte.

## 8. Ortak veri sözleşmeleri

- **Not deposu:** `<veri dizini>/graph.db`. Veri dizini: `NOTEAPP_HOME/data`, `$XDG_DATA_HOME/noteapp` (`~/.local/share/noteapp`), macOS `~/Library/Application Support/noteapp`, Windows `%LOCALAPPDATA%\noteapp`. Durum ya da önbellek değildir: senkronsuz cihazda silinmesi notların kaybıdır.
- **`notes_dir`** (varsayılan `~/notes`, Windows'ta `%USERPROFILE%\notes`): senkron deposu (`graph/<cihaz>.oplog`). Değiştirmek not listesini değiştirmez.
  Nokta ile başlayan dosya/dizinler (`.git`, `.noteapp`, geçici dosyalar) not sayılmaz.
- **Config:** `config.toml` — Windows `%APPDATA%\noteapp\`, Linux `$XDG_CONFIG_HOME/noteapp/` (`~/.config`),
  macOS `~/Library/Application Support/noteapp/`. `NOTEAPP_HOME` hepsini ezer. Şema: SPEC-core §Config.
- **Veri/durum dizini:** log + senkron durumu — Windows `%LOCALAPPDATA%\noteapp\`, Linux `$XDG_STATE_HOME/noteapp/`.
  Asla `notes_dir` içinde değil (senkronlanmaz).
- **Sırlar:** GitHub token yalnız OS kasasında (servis `noteapp`, hesap = remote host). Config'de, logda,
  URL'de, commit'te, hata mesajında **asla**.

## 9. Sınırlar

- **Her zaman:**
  - Commit öncesi tüm testler yeşil; `clang-format` uygulanmış.
  - Dışarıdan gelen her yol/ad `NotePath::parse` ile doğrulanır (notes_dir dışına çıkış imkânsız).
  - Not yazımı atomik (geçici dosya + rename); dışarıdan değişmiş notun üzerine sessizce yazılmaz.
  - Yeni kullanıcı metni hem `en.toml` hem `tr.toml`'a eklenir (anahtar eşliği testi bunu zorlar).
  - Silme, onay ister (CLI'de `-y` ile atlanabilir); silme yalnız tam yol ile (fuzzy eşleşmeyle silme yok).
- **Önce sor:**
  - §3 listesinde olmayan bağımlılık eklemek.
  - Disk formatını değiştirmek (dosya adlandırma, klasör düzeni, notes repo'daki `.gitignore` içeriği).
  - Config şemasında geriye uyumsuz değişiklik.
  - Senkron çakışma politikasını değiştirmek; CI eklemek; paketleme (rpm/msi/winget).
- **Asla:**
  - `git push --force` veya uzak geçmişi yeniden yazmak.
  - Bir notu onaysız silmek ya da çakışma kopyası bırakmadan üzerine yazmak.
  - Token'ı düz metin saklamak/loglamak; telemetri veya yapılandırılmış remote dışında ağ çağrısı.
  - Kodda Türkçe tanımlayıcı; gömülü UI metni; build dizinlerini commit'lemek.

## 10. Başarı kriterleri (bütün sistem)

1. Temiz klonda §4 komutlarıyla Windows 11 (`scripts/build-windows.sh` çapraz derleme; gerçek Windows'ta doğrulanmadı, `--test` Wine'da şu an geçmiyor, bkz. §4) ve Fedora (GCC; `scripts/check-fedora.sh` ile doğrulandı) üzerinde derlenir, tüm testler geçer.
2. `note "merhaba"` notu ≤ 200 ms içinde diske yazar (senkron arka planda, bu süreye dahil değil).
   (ölçüm 2026-10-31: medyan 6.6 ms, ilk koşu 10.6 ms — Release, Fedora konteyneri, `ctest -R perf`. Önceki Windows
   ölçümü 40–50 ms idi; Windows'ta yeniden ölçülmedi)
3. `notes` 5.000 notla ≤ 500 ms'de açılır; her tuş vuruşunda filtre ≤ 50 ms; sıcak içerik araması ≤ 100 ms.
   (ölçüm 2026-10-31, Release, Fedora konteyneri, SDL dummy sürücü: açılış vekili medyan 30.2 ms, süreç içi 23.6 ms;
   TUI tuş başına 2.1 ms; GUI tuş başına (model + görünen satır) 2.6 ms; sıcak arama 19.2 ms, soğuk tazeleme
   348.8 ms ölçüt dışı; GUI 1 MB notta tuş medyanı 16.7 ms, en kötü 19.3 ms. Gerçek soğuk disk önbelleği testte
   üretilemez; Windows'ta yeniden ölçülmedi)
4. Tip önerisi örnek korpusta ilk-tahmin doğruluğu ≥ %90; TUI ve GUI kaydet diyaloğunda tip değiştirilebilir.
5. Senkron varsayılan kapalı; açılınca HTTPS+token **veya** SSH ile Windows ↔ Fedora arasında private repo
   üzerinden notlar eşitlenir; aynı notun iki cihazda değişmesi çakışma kopyası üretir, hiçbir içerik kaybolmaz.
6. Token, `notes_dir`, config ve log dizinlerinde hiçbir dosyada geçmez (test bunu grep ile doğrular).
7. Arayüz dili tr/en arasında değiştirilebilir; iki katalog anahtar-eşit; kullanıcıya görünen metin katalog dışında yok.
8. GUI'den ve `note export --pdf` ile Türkçe karakterli not okunabilir PDF olarak dışa aktarılır.
9. GUI Ayarlar sekmesi ve TUI ayar ekranı aynı `config.toml`'u okur/yazar; birinde yapılan değişiklik diğerinde görünür.

## 11. Kapsam dışı (v1)

Etiketler, wiki-link/backlink, Markdown görüntüleme, sözdizimi renklendirme, şifreleme, mobil, ek/resim
yönetimi, çoklu kullanıcı, paketleme (rpm/msi/winget), macOS'ta elle doğrulama, CI.

## 12. Açık sorular

1. ~~Qt'nin Windows kurulumu~~ — **Karar (2026-10-28):** Qt Online Installer ile Qt 6.12.0 `mingw_64` kuruldu;
   Windows derleyicisi Qt'nin MinGW 13.1'i. **Geçersiz (2026-10-31):** Qt kaldırıldı, Windows derleyicisi
   MinGW-w64 GCC; ayrıntı aşağıdaki `## Değişiklik 2026-10-31`. Fedora doğrulaması konteynerde.
2. **SSH:** libgit2 `USE_SSH=exec` (sistemdeki OpenSSH'yi çağırır; anahtar/agent/`~/.ssh/config` otomatik)
   seçildi. Windows 11'de erken bir spike ile doğrulanacak; olmazsa `libssh2` bağımlılığı (önce sorulur).
   **Spike (2026-10-28):** exec arka ucu Windows'ta PATH'teki `ssh`'ı başlatıyor (doğrulandı); github.com host
   anahtarı known_hosts'ta olmadığından bağlantı host doğrulamasında kaldı (Git Bash ssh hata verir, Win32-OpenSSH
   onay bekleyip asılı kalır). Kontrol noktası 4: `ssh -T` onayı sonrası anahtar/agent ile `ls-remote` + asılı kalma önlemi.
3. ~~İlk senkron~~ — **Karar (spec onayıyla, 2026-10-28):** ikisi de doluysa v1 reddeder + yönlendirir.
4. ~~Kurulum~~ — **Karar (spec onayıyla, 2026-10-28):** v1'de paketleme yok; `cmake --install` + PATH elle.

## Değişiklik 2026-10-31

Qt 6 kaldırıldı; GUI SDL3 + Dear ImGui'ye taşındı. Karar kaydı: `tasks/plan-imgui.md`. Ayrıntılar SPEC-gui, SPEC-cli.

- **MODIFIED** §3: GUI/PDF bileşenleri Qt'den SDL3, Dear ImGui, libharu, md4c ve gömülü DejaVu'ya geçti (pin ve lisanslar tabloda).
  Neden: toolkit kararı kullanıcının (`tasks/plan-imgui.md`).
- **MODIFIED** Windows toolchain: Qt'nin MinGW 13.1'i yerine MinGW-w64 GCC; `NOTEAPP_QT_DIR` ve `windeployqt` kalktı.
  Windows ikilileri Linux'ta konteynerde çapraz derlenir (`scripts/build-windows.sh`, Fedora `mingw64-gcc-c++` 16.2.1, statik,
  yalnız Windows sistem DLL'leri). Windows akışı gerçek Windows'ta doğrulanmadı.
- **ADDED** §2 `pdf` modülü (`noteapp_pdf`): `cli` ve `gui` doğrudan link olur. Neden: `note export --pdf` GUI'siz derlemede
  ve ekransız sunucuda çalışsın.
- **REMOVED** `cli` -> `notes-gui` çalışma zamanı çağrısı (`notes-gui --export-pdf`).
- **MODIFIED** §4/§7: `scripts/check-fedora.sh` docker, yoksa podman kullanır; GUI testleri `SDL_VIDEO_DRIVER=dummy` ile başsız.
- **MODIFIED** §10/1–3: ölçümler Fedora konteynerinde yenilendi (Windows'ta yeniden ölçülmedi).
- **MODIFIED** §12/1: Qt Windows kurulum kararı geçersiz (satır tarihsel not olarak duruyor, numara korundu).
- **MODIFIED** Windows toolchain (bu sürüm): MSYS2 MinGW-w64 kullanımı bırakıldı; çapraz derleme birincil, yerel derleme PATH'teki
  herhangi bir MinGW-w64 `g++` ile (doğrulanmadı). `--test` Wine'da şu an sıfırdan farklı çıkar (bkz. §4).
- **ADDED** `scripts/build-appimage.sh` (Linux AppImage), `scripts/build-windows.sh`, `scripts/container-engine.sh`,
  `docker/windows-cross.Containerfile`, `cmake/toolchains/mingw-w64-x86_64.cmake`, preset'ler `windows-cross-{debug,release}`.
- **REMOVED** MSYS2 bağımlılığı ve `scripts/dev-env.sh` (`MSYS2_PREFIX`); MinGW DLL kopyalama riski statik bağlamayla kalktı.
