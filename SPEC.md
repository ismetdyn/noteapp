# Spec: noteapp — çapraz platform not uygulaması

> Durum: **ONAYLANDI** · Tarih: 2026-10-28
> Bu dosya yetenek haritası + tüm modüllerin ortak kurallarıdır. Her modülün ayrıntısı
> `SPEC-<modül-id>.md` dosyasındadır; hangi spec'lerin var olduğunun tek dizini aşağıdaki haritadır.

## 1. Amaç

Terminalde yaşayan biri için hızlı, düz dosyalı, sahibine ait bir not sistemi:

- `note "metin"` ile saniyesinde not ekle; ya da `note` deyip sevdiğin editörde (edit/micro/nano/vim…) yaz.
- `notes` ile TUI aç: solda fzf hissiyle notlar arası gezin, sağda içeriği gör.
- `notes-gui` ile aynı notları masaüstü penceresinde oluştur/düzenle/gez; Ayarlar sekmesinden yapılandır.
- İstenirse (aç/kapa) notlar private bir GitHub reposuyla senkron kalsın; Windows ↔ Fedora arasında.

**Kullanıcı:** tek kişi (geliştirici), iki cihaz: Windows 11 (iş) ve Fedora Linux (ev).

**Tasarım ilkeleri (araştırmadan: nb, Joplin, Trilium, QOwnNotes, zk, nvALT):**
- Notlar **düz dosya**; kimlik = notes_dir'e göre göreli yol, tip = dosya uzantısı. Ayrı veritabanı yok,
  metadata dosya dışında tutulmaz (QOwnNotes'un etiket-DB tuzağı).
- Arama kutusu aynı zamanda oluşturma kutusudur (nvALT): yazdıkça filtrele, eşleşme yoksa Enter yeni not.
- Kaydetmede veri kaybı yok: atomik yazma, dışarıdan değişiklik tespiti, senkron çakışmasında iki kopya.
- Ayarlar az ve tek dosyada; CLI/TUI/GUI aynı dosyayı paylaşır; sırlar asla bu dosyada değil.

## 2. Yetenek haritası (onaylandı: 2026-10-28)

| Modül id | Sorumluluk | Bağımlı olduğu | Spec |
|---|---|---|---|
| `core` | Not deposu (not=dosya, klasör=dizin, tip=uzantı), yol güvenliği, atomik yazma, config, i18n (tr/en), editör başlatma, platform dizinleri | — | [SPEC-core.md](SPEC-core.md) |
| `detect` | İçerikten tip önerisi (puanlı sezgisel) | — | [SPEC-detect.md](SPEC-detect.md) |
| `search` | fzf tarzı ad/yol eşleştirme + tam metin içerik arama | core | [SPEC-search.md](SPEC-search.md) |
| `sync` | libgit2 ile GitHub senkronu; aç/kapa; HTTPS+token ve SSH; çakışmada iki kopya | core | [SPEC-sync.md](SPEC-sync.md) |
| `cli` | `note` komutu: hızlı ekleme, ls/find/show/edit/rm/mv/export/sync/config | core, detect, search, sync | [SPEC-cli.md](SPEC-cli.md) |
| `tui` | `notes`: fzf tarzı liste + önizleme, kaydet diyaloğu, ayar ekranı | core, detect, search, sync | [SPEC-tui.md](SPEC-tui.md) |
| `gui` | `notes-gui`: klasör ağacı + liste + editör, Ayarlar sekmesi, PDF dışa aktarma | core, detect, search, sync | [SPEC-gui.md](SPEC-gui.md) |

**Yapım sırası:** `core` ∥ `detect` → `search` ∥ `sync` → `cli` → `tui` → `gui`
(`cli`/`tui`/`gui` birbirinden bağımsız; sıra değer teslimine göre.)

Bağımlılık yönü tek yönlü; döngü yok. `cli` → `gui` arasında **link bağımlılığı yoktur**; `note export --pdf`
çalışma zamanında `notes-gui --export-pdf` sürecini çağırır (bkz. SPEC-cli, SPEC-gui).

## 3. Teknoloji

| Bileşen | Seçim | Sürüm (pin) | Lisans | Nereden |
|---|---|---|---|---|
| Dil | C++23 (`std::expected`, `std::filesystem`) | — | — | — |
| Derleme | CMake + Ninja, `CMakePresets.json` | CMake ≥ 3.28 | — | sistem / CLion |
| Derleyici | Windows: Qt ile gelen MinGW-w64 GCC 13.1 (`C:\Qt\Tools\mingw1310_64`), Fedora: GCC ≥ 14, macOS: Apple Clang | — | — | sistem |
| TUI | FTXUI | v7.0.3 | MIT | FetchContent |
| GUI + PDF | Qt 6 Widgets (+ QtGui `QPdfWriter`) — Windows'ta 6.12.0 `mingw_64` kiti | ≥ 6.8 (LTS) | LGPLv3 (dinamik link) | sistem (Qt Online Installer) |
| Config / i18n | toml++ (`TOML_EXCEPTIONS=0`) | v3.4.0 | MIT | FetchContent |
| Git senkron | libgit2 (`USE_SSH=exec`, HTTPS: WinHTTP / OpenSSL / SecureTransport) | v1.9.7 | GPLv2 + linking exception | FetchContent |
| Sır saklama | hrantzsch/keychain (Win Credential Manager / libsecret / macOS Keychain) | v1.3.1 | MIT | FetchContent |
| Test | doctest | v2.5.3 | MIT | FetchContent |
| Fuzzy algoritma | fzy algoritması (jhawthorn/fzy) — C++'ta yeniden yazım | ref. v1.1 | MIT | — |

- Windows'ta **tüm proje** Qt'nin kendi MinGW 13.1'iyle derlenir (Qt ikilileriyle birebir ABI/çalışma zamanı
  uyumu). CLion'un GCC 15'i kullanılmaz: Qt'nin yanındaki eski `libstdc++-6.dll` ile çalışma zamanı çakışması riski.
  GCC 13 sınırı: `std::expected`/`std::format` var, `std::print` yok → kullanılmaz.
- MSVC bu makinede kurulu değil (VS 2026'da C++ iş yükü yok); gerekirse ileride preset eklenir.
- Makineye özel yollar (Qt, MinGW) commit'lenmez: `CMakeUserPresets.json` (gitignore'da) veya ortam değişkeni.
- Fedora sistem paketleri: `gcc-c++ cmake ninja-build qt6-qtbase-devel libsecret-devel openssl-devel pkgconf-pkg-config`.
- Listede olmayan her yeni bağımlılık **önce sorulur** (§9).

## 4. Komutlar

```powershell
# Windows — MinGW + Qt yolları CMakeUserPresets.json'dan (bkz. README) ya da:
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\6.12.0\mingw_64\bin;$env:PATH"
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.12.0\mingw_64"
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug --output-on-failure
ctest --preset windows-mingw-debug -R "core"          # tek modül testleri
cmake --build --preset windows-mingw-release
cmake --install build/windows-mingw-release --prefix "$env:LOCALAPPDATA\Programs\noteapp"   # + windeployqt
```

```bash
# Fedora doğrulaması bu makinede (Docker, kontrol noktalarında)
scripts/check-fedora.sh            # fedora:latest konteynerinde derle + ctest (GUI testleri offscreen)
```

```bash
# Fedora
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel libsecret-devel openssl-devel pkgconf-pkg-config
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
CMakeLists.txt            → kök; alt dizinleri ekler, seçenekler: NOTEAPP_BUILD_GUI (ON), NOTEAPP_BUILD_TESTS (ON)
CMakePresets.json         → windows-mingw-{debug,release}, linux-gcc-{debug,release}, macos-clang-{debug,release}
scripts/check-fedora.sh   → Docker'da Fedora derleme + test (kontrol noktası doğrulaması)
docker/fedora.Containerfile → Fedora derleme imajı (bağımlılıklar önbellekli)
cmake/Dependencies.cmake  → tüm FetchContent pin'leri tek yerde
cmake/EmbedResources.cmake→ resources/* dosyalarını derleme anında ikiliye gömer
resources/i18n/en.toml    → İngilizce mesaj kataloğu (kaynak dil)
resources/i18n/tr.toml    → Türkçe mesaj kataloğu
resources/defaults.toml   → varsayılan config (tip listesi, varsayılan tip, süreler…)
src/core/                 → noteapp_core (statik kütüphane)
src/detect/               → noteapp_detect
src/search/               → noteapp_search
src/sync/                 → noteapp_sync
src/cli/                  → `note` çalıştırılabilir
src/tui/                  → `notes` çalıştırılabilir
src/gui/                  → `notes-gui` çalıştırılabilir (Qt)
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
    metne render eden TUI testleri, `QT_QPA_PLATFORM=offscreen` ile GUI model/diyalog testleri.
  - *Large (~%5, elle):* gerçek GitHub reposuyla Windows ↔ Fedora senkronu, gerçek editörlerle akış,
    GUI elle kontrol listesi (her modül spec'indeki "Elle doğrulama" bölümü).
- **Kritik yollar** (veri kaybı/güvenlik): yol kaçışı, atomik yazma, dışarıdan değişiklik, senkron çakışması,
  token sızıntısı — bunlar için önce kırmızı test (TDD), her edge-case ayrı test.
- **Kapsam:** `core`, `detect`, `search`, `sync` mantığında hedef ≥ %80 satır kapsamı (Fedora'da gcov ile ölçülür;
  zorunlu kapı değil, düşüşte uyarı). Ön yüzlerde kapsam hedefi yok; davranış testleri + elle liste.
- Testler ağa çıkmaz; GitHub'a gerçek erişim yalnız elle (Large) testte.

## 8. Ortak veri sözleşmeleri

- **Not deposu:** `notes_dir` (varsayılan `~/notes`, Windows'ta `%USERPROFILE%\notes`). Alt dizinler = klasörler.
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

1. Temiz klonda §4 komutlarıyla Windows 11 (MSVC) ve Fedora (GCC) üzerinde derlenir, tüm testler geçer.
2. `note "merhaba"` notu ≤ 200 ms içinde diske yazar (senkron arka planda, bu süreye dahil değil).
3. `notes` 5.000 notla ≤ 500 ms'de açılır; her tuş vuruşunda filtre ≤ 50 ms; sıcak içerik araması ≤ 100 ms.
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
   Windows derleyicisi Qt'nin MinGW 13.1'i (MSVC kurulu değil). Fedora doğrulaması Docker konteynerinde.
2. **SSH:** libgit2 `USE_SSH=exec` (sistemdeki OpenSSH'yi çağırır; anahtar/agent/`~/.ssh/config` otomatik)
   seçildi. Windows 11'de erken bir spike ile doğrulanacak; olmazsa `libssh2` bağımlılığı (önce sorulur).
3. ~~İlk senkron~~ — **Karar (spec onayıyla, 2026-10-28):** ikisi de doluysa v1 reddeder + yönlendirir.
4. ~~Kurulum~~ — **Karar (spec onayıyla, 2026-10-28):** v1'de paketleme yok; `cmake --install` + PATH elle.
