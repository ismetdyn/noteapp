# noteapp

Terminalde yaşayan biri için hızlı, düz dosyalı, sahibine ait bir not sistemi. Notlar `notes_dir` altında sıradan
dosyalardır (not = dosya, klasör = dizin, tip = uzantı); ayrı veritabanı yoktur. Üç ön yüz aynı notları ve aynı
ayar dosyasını paylaşır: `note` (CLI), `notes` (TUI), `notes-gui` (Qt 6 masaüstü). İstenirse notlar private bir
GitHub reposuyla senkron kalır (Windows ↔ Linux).

## Özellikler

- `note "metin"` ile saniyesinde ekleme; stdin'den ya da sevdiğin editörden (`-e`) yazma
- İçerikten tip önerisi (md, java, py, json…); notun tipi dosya uzantısıdır
- fzf tarzı bulanık arama (Türkçe katlama dahil) ve tam metin içerik araması
- `notes`: iki panelli TUI (liste + önizleme), nvALT mantığı: eşleşme yoksa Enter yeni not
- `notes-gui`: klasör ağacı + liste + editör, otomatik kayıt, ayarlar sekmesi, PDF dışa aktarma
- Veri kaybı yok: atomik yazma, dışarıdan değişiklik tespiti, onaysız silme yok
- Aç/kapa GitHub senkronu (libgit2; HTTPS+token ya da SSH); çakışmada iki kopya, force push yok
- Türkçe/İngilizce arayüz, tek `config.toml`; sırlar yalnız OS kasasında

## Kaynaktan kurulum

Gerekenler: C++23 derleyici, CMake ≥ 3.28, Ninja, Qt 6 (≥ 6.8, yalnız GUI için). FTXUI, libgit2, toml++, doctest
vb. derleme sırasında otomatik indirilir (`cmake/Dependencies.cmake`).

### Windows 11

1. Qt Online Installer ile **Qt 6.12.0 → `MinGW 13.1.0 64-bit`** kitini ve **Developer and Designer Tools →
   MinGW 13.1.0 64-bit**, Ninja, CMake bileşenlerini kur (varsayılan yer `C:\Qt`). Tüm proje Qt'nin kendi MinGW'siyle
   derlenir; CLion'un GCC'si kullanılmaz.
2. Git Bash'te:

```bash
source scripts/dev-env.sh            # PATH + NOTEAPP_QT_DIR; QT_ROOT / QT_VERSION / QT_MINGW ile ezilebilir
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

**CLion:** Toolchain = `C:\Qt\Tools\mingw1310_64`; CMake profilinin ortamına `NOTEAPP_QT_DIR=C:/Qt/6.12.0/mingw_64`
ekle; preset olarak `windows-mingw-debug` seç.

### Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel libsecret-devel openssl-devel pkgconf-pkg-config
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
```

### Kurulum (install) ve PATH

```bash
# Windows (Git Bash) — windeployqt adımı otomatik çalışır; Qt/MinGW DLL'leri bin\ içine konur
cmake --install build/windows-mingw-release --prefix "$LOCALAPPDATA/Programs/noteapp"
# Linux
cmake --install build/linux-gcc-release --prefix ~/.local
```

`bin` dizinini PATH'e ekle: Windows'ta `%LOCALAPPDATA%\Programs\noteapp\bin` (Ayarlar → Ortam Değişkenleri →
kullanıcı `Path`), Linux'ta `~/.local/bin`. Kurulu `bin` kendi içinde eksiksizdir (Qt DLL'leri dahil), makinede Qt
ya da MinGW gerekmez. `notes` ve `notes-gui` `note`'u, `note export --pdf` `notes-gui`'yi **kendi yanında** arar;
üçünü ayrı dizinlere dağıtma.

## Kullanım

### `note` (CLI)

```bash
note "toplantı notu: bütçe onayı"          # hızlı ekleme (tip içerikten önerilir)
echo "SELECT 1;" | note -t txt             # stdin'den, tipi zorla
note -f work/2026 -n plan "ilk taslak"     # klasör + ad → work/2026/plan.md
note                                       # metin yok: editör açılır; boş bırakırsan iptal
note -e "Başlık"                           # editörü ön içerikle aç
note ls [SORGU] [-f KLASÖR]                # listele (sorgu: bulanık sıra, yoksa en yeni önce)
note find bütçe onay                       # içerik araması → yol:satır: kesit
note show SORGU | note edit SORGU          # en iyi eşleşmeyi yazdır / editörde aç
note mv KAYNAK HEDEF                       # taşı/yeniden adlandır; uzantı değişimi = tip değişimi
note rm YOL [-y]                           # yalnız tam yol; TTY yoksa -y şart
note export --pdf YOL [-o ÇIKTI]           # PDF (notes-gui gerekir)
note sync [status | on | off | init REMOTE [--replace] | login [HOST] | logout [HOST]]
note config path | list | get ANAHTAR | set ANAHTAR DEĞER
note -- ls                                 # "ls" metnini not et (alt komut adıyla çakışma)
```

`--no-sync` yazan komutlarda arka plan senkronunu atlar. Çıkış kodları: `0` başarı · `1` kullanıcı kaynaklı (iptal,
bulunamadı) · `2` kullanım hatası · `3` G/Ç/iç hata · `4` senkron hatası.

### `notes` (TUI)

| Tuş | Eylem |
|---|---|
| yazı | Sorguyu düzenler, liste anında süzülür |
| `↑` `↓` `Ctrl-P` `Ctrl-N` | Listede gezin |
| `PgUp` `PgDn` | Önizlemeyi kaydır |
| `Tab` | Ad ↔ içerik modu |
| `Enter` | Notu editörde aç; eşleşme yoksa sorgu ilk satır olacak şekilde yeni not |
| `Ctrl-O` | Boş yeni not |
| `Ctrl-R` | Ad/klasör/tip diyaloğu (taşı/yeniden adlandır) |
| `Ctrl-D` | Sil (onaylı, varsayılan "Vazgeç") |
| `Ctrl-S` | Şimdi senkronla |
| `F2` / `F1` | Ayarlar / yardım |
| `Esc` | Sorguyu temizle, boşsa çık · `Ctrl-C` çık |

En küçük pencere 60×15.

### `notes-gui`

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (`↑↓` listede gezer, `Enter` açar) |
| `Ctrl+N` / `Ctrl+S` | Yeni not / şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu |
| `Ctrl+M` | Klasöre taşı |
| `Del` (listede) | Sil (onaylı) |
| `Ctrl+E` | PDF dışa aktar |
| `F5` | Şimdi senkronla |
| `Ctrl+,` | Ayarlar sekmesi |

Mevcut notlar yazmayı bıraktıktan 1 sn sonra otomatik kaydedilir. Başsız PDF:
`notes-gui --export-pdf <girdi> <çıktı>` (`QT_QPA_PLATFORM=offscreen` ile pencere açmaz).

## Yapılandırma

| | Windows | Linux |
|---|---|---|
| `config.toml` | `%APPDATA%\noteapp\` | `$XDG_CONFIG_HOME/noteapp/` (`~/.config/noteapp/`) |
| log + senkron durumu | `%LOCALAPPDATA%\noteapp\` | `$XDG_STATE_HOME/noteapp/` |
| notlar (`notes_dir`) | `%USERPROFILE%\notes` | `~/notes` |

`NOTEAPP_HOME` ayarlıysa config `$NOTEAPP_HOME/config`, durum `$NOTEAPP_HOME/state` altına gider (deneme/test için
izole ortam). Ayarlar `note config`, TUI'de `F2`, GUI'de Ayarlar sekmesinden değişir; dosyayı elle de düzenleyebilirsin.

| Anahtar | Anlamı |
|---|---|
| `language` | `auto` / `tr` / `en` |
| `notes_dir` | Notların kökü (`~` ve ortam değişkenleri genişler) |
| `editor` | Boşsa `$VISUAL` → `$EDITOR` → Windows'ta `edit`/`notepad` |
| `default_type` | Tipi belirsiz tek satırlık notun uzantısı (`md`) |
| `default_folder` | Hızlı notların klasörü (boş = kök) |
| `types` | Tip seçim listesi (elle başka uzantı da yazılabilir) |
| `sync.enabled` | Senkron açık/kapalı |
| `sync.remote` | `https://…` (token) ya da `git@…` (SSH) |
| `sync.branch` | Dal (`main`) |
| `sync.idle_seconds` | TUI/GUI'de son değişiklikten kaç sn sonra senkronlanır |
| `sync.author_name` / `sync.author_email` | Commit yazarı (boşsa git config → OS kullanıcısı) |

Bozuk `config.toml` uygulamayı çökertmez (varsayılanlar + log uyarısı), `note config set` bozuk dosyada reddeder.

## Senkron kurulumu (GitHub)

1. GitHub'da **boş ve private** yeni bir repo aç (ör. `notes`). Kod reposunu **asla** not reposu olarak kullanma.
2. **HTTPS + token:** Settings → Developer settings → *Fine-grained personal access token*; yalnız bu repoyu seç,
   izin: **Contents → Read and write**.
3. Remote'u bağla ve token'ı kasaya yaz:

```bash
note sync init https://github.com/<kullanici>/notes.git
note sync login                      # token'ı gizli ister (ekrana yansımaz); Windows Credential Manager'a gider
```

   **SSH alternatifi:** anahtarını GitHub hesabına ekle, bir kez `ssh -T git@github.com` çalıştırıp host anahtarını
   kabul et, sonra `note sync init git@github.com:<kullanici>/notes.git`. Uygulama anahtar saklamaz; `ssh`, agent ve
   `~/.ssh/config` kullanılır.
4. `note sync status` açık/kapalı, remote, son başarı ve son hatayı gösterir; `note sync` elle tek döngü çalıştırır.

Başlangıçta `notes_dir` boşsa repo klonlanır; dolu ve remote boşsa notlar ilk commit olarak gider. İkisi de doluysa
`sync.both_non_empty` ile reddedilir (içerik kaybı olmasın diye).

Sonrasında not ekleyen/değiştiren her komut arka planda senkronu başlatır; TUI/GUI açılışta, değişikliklerden sonra
(`idle_seconds`) ve çıkışta senkronlar. **Çakışma** (aynı not iki cihazda değişti): yerel sürüm yerinde kalır, uzak
sürüm yanına `ad (conflict YYYY-MM-DD HHMM).md` olarak yazılır, hiçbir şey kaybolmaz; kopyayı elle birleştirip silersin.

Kapatmak: `note sync off` (repo ve geçmiş olduğu gibi kalır); token'ı silmek için `note sync logout`.

## Sorun giderme

- **PowerShell 5.1'de Türkçe metin boruya girince bozuluyor:** oturumda
  `$OutputEncoding = [Text.Encoding]::UTF8` çalıştır; mümkünse Windows Terminal kullan.
- **`sync.ssh_host_unknown`:** host anahtarı henüz bilinmiyor; bir kez `ssh -T git@github.com` çalıştırıp onayla.
- **Linux'ta `sync.keychain_unavailable` (Secret Service yok):** token düz dosyaya yazılmaz; ortama ver:
  `export NOTEAPP_GIT_TOKEN=<token>`. Bu token yalnız `NOTEAPP_GIT_TOKEN_HOST` adresine (varsayılan `github.com`)
  gönderilir; başka bir sunucu için bu değişkeni de ayarlayın.
- **`cli.gui_not_installed`:** `note export --pdf` yanındaki `notes-gui`'yi bulamadı; `cmake --install` ile üçünü
  aynı `bin`'e kur.
- **Senkron kilitli (`sync.busy`):** başka bir senkron sürüyor; bekle ya da tekrar dene. Ayrıntı için log
  (`…/noteapp/log.txt`).

## Geliştirme

```bash
ctest --preset windows-mingw-debug --output-on-failure     # Linux: linux-gcc-debug
ctest --preset windows-mingw-debug -R "core"               # tek modül
scripts/check-fedora.sh                                    # Fedora'da derle + test (Docker)
clang-format -i $(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp')
```

Spec'ler: [SPEC.md](SPEC.md) (harita, teknoloji, sözleşmeler) ve modül spec'leri
[core](SPEC-core.md) · [detect](SPEC-detect.md) · [search](SPEC-search.md) · [sync](SPEC-sync.md) ·
[cli](SPEC-cli.md) · [tui](SPEC-tui.md) · [gui](SPEC-gui.md). Plan: [tasks/plan.md](tasks/plan.md),
görevler: [tasks/todo.md](tasks/todo.md).
