# noteapp

Terminalde yaşayan biri için hızlı, sunucusuz, sahibine ait bir not sistemi. Notlar veri dizininde tek bir gömülü
veritabanındadır (`graph.db`); baytlar aynen durur, `note export --all` hepsini düz dosya olarak geri verir. Üç ön yüz aynı notları ve aynı
ayar dosyasını paylaşır: `note` (CLI), `notes` (TUI), `notes-gui` (SDL3 + Dear ImGui masaüstü). İstenirse notlar private bir
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

Gerekenler: C++23 derleyici, CMake ≥ 3.28, Ninja. FTXUI, libgit2, toml++, SDL3, Dear ImGui, libharu, md4c, doctest
vb. derleme sırasında otomatik indirilir (`cmake/Dependencies.cmake`); ağ erişimi gerekir. Fontlar (DejaVu) depoda gömülüdür.

CMake seçenekleri: `NOTEAPP_BUILD_GUI` (varsayılan ON; "notes-gui (SDL3 + Dear ImGui) derlensin") ve `NOTEAPP_BUILD_TESTS`
(varsayılan ON). GUI'siz derleme için `-DNOTEAPP_BUILD_GUI=OFF`: `note` (PDF dışa aktarma dahil) ve `notes` derlenir.

### Windows 11

> Windows akışı gerçek Windows'ta henüz çalıştırılmadı. İmzasız `.exe`'ler SmartScreen uyarısı verebilir.

**Linux'ta çapraz derleme (önerilen).** Host'ta yalnız podman ya da docker gerekir (docker servisi çalışmıyorsa podman seçilir,
bkz. `scripts/container-engine.sh`):

```bash
bash scripts/build-windows.sh
```

Çıktı: `out/windows-x86_64/{note,notes,notes-gui}.exe` ve bileşen lisansları `licenses/`; dağıtırken ikisini birlikte
kopyala. İkililer statiktir; kopyalayıp çalıştırılır, yalnız Windows sistem DLL'lerini içe aktarır (betik başka DLL görürse
hata verir). `version`, `winmm`, `winhttp`, `crypt32` gecikmeli yüklenir ve DLL araması System32 ile sınırlanır; exe yanına
bırakılan aynı adlı DLL'in yüklenmediğini betik Wine'da sınar. Gerçek Windows'ta sınanmadı: exe'leri İndirilenler yerine kendi
klasöründen çalıştır. Derleyici imajdaki Fedora `mingw64-gcc-c++` 16.2.1'dir;
ikililer `msvcrt.dll`'e bağlanır. İmaj tanımı: `docker/windows-cross.Containerfile`, toolchain:
`cmake/toolchains/mingw-w64-x86_64.cmake` (preset `windows-cross-release`).

`bash scripts/build-windows.sh --test [ctest argümanları]` önce testleri Wine'da koşar, sonra ikilileri üretir. Bu koşu
şu an sıfırdan farklı kodla biter:

- core `hidden note stays hidden`: Wine sınırı.
- sync `a push the remote keeps rejecting`: aralıklı düşer, neden çözülmedi; gerçek Windows'ta doğrulanacak.

**Windows'ta yerel derleme (doğrulanmadı).** PATH'te herhangi bir MinGW-w64 `g++`, CMake ≥ 3.28 ve Ninja olmalı; ek ortam
kurulumu ve DLL kopyalama yok. Test derlemesi PATH'te `pdftotext.exe` ister (eksikse yapılandırma durur; testsiz derleme için
`-DNOTEAPP_BUILD_TESTS=OFF`):

```bash
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

**macOS:** yalnız Mac üzerinde derlenir (preset `macos-clang-*`); çapraz derleme yok.

### Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build git libsecret-devel openssl-devel pkgconf-pkg-config \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel \
    libXtst-devel dbus-devel ibus-devel libxkbcommon-devel mesa-libGL-devel mesa-libEGL-devel mesa-libGLES-devel \
    wayland-devel wayland-protocols-devel libdecor-devel poppler-utils
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
```

Liste `docker/fedora.Containerfile` ile aynıdır (SDL3 X11/Wayland/D-Bus arka uçları için). Testler varsayılan olarak açıktır ve
`poppler-utils`'teki `pdftotext`'i ister; eksikse yapılandırma durur. Testsiz derleme: `-DNOTEAPP_BUILD_TESTS=OFF`.

### Kurulum (install) ve PATH

```bash
# Windows, yerel derleme (doğrulanmadı; çapraz derlemede .exe'leri bir dizine kopyalamak yeter)
cmake --install build/windows-mingw-release --prefix "$env:LOCALAPPDATA/Programs/noteapp"
# Linux
cmake --install build/linux-gcc-release --prefix ~/.local
```

Kurulum yalnız `bin/note`, `bin/notes`, `bin/notes-gui` koyar (Linux'ta doğrulandı).
`bin` dizinini PATH'e ekle: Windows'ta `%LOCALAPPDATA%\Programs\noteapp\bin` (Ayarlar → Ortam Değişkenleri →
kullanıcı `Path`), Linux'ta `~/.local/bin`. `notes` ve `notes-gui` `note`'u **kendi yanında** arar (arka plan
senkronu için); üçünü ayrı dizinlere dağıtma.

### Linux AppImage

Tek dosyada üç komut. Host'ta yalnız docker ya da podman gerekir; çıktı `out/noteapp-<sürüm>-x86_64.AppImage`:

```bash
bash scripts/build-appimage.sh
```

Çalıştırma için glibc ≥ 2.38 ve FUSE gerekir (Fedora: `sudo dnf install fuse3`). Argümansız `notes-gui` açılır; ilk argüman
`note`, `notes` ya da `gui` komutu seçer. Komutları PATH'e symlink ile koy (dosya adı komutu belirler):

```bash
install -m 755 out/noteapp-*-x86_64.AppImage ~/.local/bin/noteapp.AppImage
for command in note notes notes-gui; do ln -sf noteapp.AppImage ~/.local/bin/"$command"; done
```

FUSE yoksa paketi özel bir dizine açıp oradan çalıştır:

```bash
appimage="$(realpath out/noteapp-*-x86_64.AppImage)"
dir="$(mktemp -d)" && (cd "$dir" && "$appimage" --appimage-extract >/dev/null)
"$dir/squashfs-root/AppRun" note ls
```

Paylaşılan makinede `--appimage-extract-and-run` ve `APPIMAGE_EXTRACT_AND_RUN=1` kullanma: çalışma zamanı paketi
`/tmp/appimage_extracted_<md5>` altına açar, dizinin sahibine bakmaz ve boyutu tutan dosyayı yeniden yazmaz; başka bir yerel
kullanıcı dizini önceden yaratıp kendi `AppRun`'ını senin kimliğinle çalıştırabilir. Zorunluysa `TMPDIR`'i yalnız sana ait
bir dizine ver ve komutla aynı satırda yaz; ayrı satırdaki `TMPDIR=...` export edilmediği için çalışma zamanına ulaşmaz:

```bash
mkdir -p -m 700 ~/.cache/noteapp-tmp && TMPDIR=~/.cache/noteapp-tmp "$(realpath out/noteapp-*-x86_64.AppImage)" --appimage-extract-and-run note ls
```

HTTPS senkronu host'un CA paketini kullanır; `SSL_CERT_FILE`/`SSL_CERT_DIR` ayarlıysa onlar geçerlidir. Token yalnız
host'un Secret Service'ine yazılır. GUI, X11/Wayland kütüphanelerini (`libX11`, `libwayland-client`, `libxkbcommon`, varsa
`libdecor`) host'tan yükler. Flatpak içinde çalışmayı reddeder: libsecret orada token'ı dosyaya yazardı. Bileşen lisansları
paket içinde `usr/share/licenses/noteapp/` altındadır; AppImage çalışma zamanının değiştirilmesi
`appimage-type2-runtime/runtime-NOTICE.txt`'te anlatılır.

## Kullanım

### `note` (CLI)

```bash
note "toplantı notu: bütçe onayı"          # hızlı ekleme (tip içerikten önerilir)
echo "SELECT 1;" | note -t txt             # stdin'den, tipi zorla
note -f work/2026 -n plan "ilk taslak"     # klasör + ad → work/2026/plan.md
note -n deneme.cpp "int x;"                # yazılan uzantı aynen kalır: deneme.cpp, a.tar.gz, notlar.v2
note                                       # metin yok: editör açılır; boş bırakırsan iptal
note -e "Başlık"                           # editörü ön içerikle aç
note ls [SORGU] [-f KLASÖR]                # listele (sorgu: bulanık sıra, yoksa en yeni önce)
note find bütçe onay                       # içerik araması → yol:satır: kesit
note show SORGU | note edit SORGU          # en iyi eşleşmeyi yazdır / editörde aç
note mv KAYNAK HEDEF                       # taşı/yeniden adlandır; uzantı değişimi = tip değişimi
note rm YOL [-y]                           # yalnız tam yol; TTY yoksa -y şart
note export --pdf YOL [-o ÇIKTI]           # PDF (A4; GUI ve ekran gerekmez)
note export --all DİZİN                    # tüm notlar bayt bayt düz dosya, çöp .trash/ altında; üzerine yazmaz, bağdan geçmez; ulaşılamayan not varsa çıkış 1
note graph migrate [--skip-failed]         # eski düz dosya notları içe aktar (aşağıda)
note graph export --format jsonl [-o DOSYA] | note graph import [--overwrite] DOSYA   # tüm graph'ı taşı
note sync [status | on | off | init REMOTE [--replace] | login [HOST] | logout [HOST]]
note config path | list | get ANAHTAR | set ANAHTAR DEĞER
note -- ls                                 # "ls" metnini not et (alt komut adıyla çakışma)
```

`--no-sync` yazan komutlarda arka plan senkronunu atlar. Çıkış kodları: `0` başarı · `1` kullanıcı kaynaklı (iptal,
bulunamadı) · `2` kullanım hatası · `3` G/Ç/iç hata · `4` senkron hatası.

### `notes` (TUI)

| Tuş | Eylem |
|---|---|
| `↑` `↓` `j` `k` | Listede gezin, sağ panel canlı izler |
| `→` `l` | Önizleme (REVIEW, `cat` gibi): `↑↓` `PgUp/PgDn` `g` `G` kaydır, `/` notta bul, `n` `N` gez, `←` `Esc` liste, `q` çık |
| `Enter` `e` | Düzenle (EDIT): `Ctrl+S` kaydet, `Ctrl+Q` çık (kaydedilmemişi sorar), `Ctrl+F` bul, `Ctrl+Z/Y`, `Ctrl+X/C/V`; `Esc` yalnız aramayı kapatır ya da seçimi kaldırır, tek başına çıkmaz |
| `E` | Harici editörde aç (`$EDITOR`) |
| `/` | Alt satırda ara: `↑↓` seç, `Enter` seçimi tut (eşleşme yoksa taslak), `Tab` ad/içerik, `Esc` kapat |
| `f` | Ad ve içerikte ara: eşleşen satır yolun altında, `Enter` REVIEW'da eşleşmeye kayar. `/` ve `f` argüman alır: `sort:-size` `type:md` `in:work/` `size:>1mb` `updated:7d` `sync:modified`; bozuk değer metin sayılmaz, yeriyle hata gösterir |
| `n` `Ctrl-O` | Yeni not: tüm terminali alan boş editör. Ad ilk `Ctrl+S`'te altta sorulur (`klasör/ad.uzantı`, önerisi ilk satır + algılanan tip), o ana dek dosya yok; `Ctrl+Q` iki panele döner |
| `a` | Yeni not, önce ad: listenin başında satır açılır, `Enter` boş notu oluşturup EDIT'te açar (yazmaya ya da yapıştırmaya hazır), `Esc` vazgeçer; uzantı yoksa `default_type`; dolu ya da geçersiz ad satırda hata verir · `r` taşı/ad/tip · `s` senkron |
| `d` `u` `D` | `d` çöpe atar (sormaz), `u` bu oturumda son atılanı geri alır, `D` kalıcı siler (sorar, varsayılan Vazgeç) |
| `F2` | Yol ve ad: altta tek satır, imleç adın sonunda; `Enter` taşır (hedef varsa hata, ezme yok), `Esc` vazgeçer |
| `T` | Çöp görünümü (ya da üstteki `çöp: N`e tıkla): `u` geri getir, `D` kalıcı sil, `E` tümünü boşalt (ikisi de sorar, varsayılan Vazgeç), `q` `Esc` geri; sağda not salt okunur |
| `c` | Notun özgün baytlarını sistem panosuna kopyalar (liste ve REVIEW); ikili ya da UTF-8 olmayan notta nedenini söyler |
| `Space` `i` / `I` | Seçili satırın / tüm satırların ayrıntısını aç-kapat; işarete (`▸` `▾`) tıklamak da olur |
| `PgUp` `PgDn` `g` `G` | Listede sayfa sayfa, baş, son; REVIEW'da `↑↓` basılı tutunca hızlanır, `Ctrl+U` `Ctrl+D` yarım sayfa, `J` `K` 5 satır |
| `Alt+→` `Alt+←` | Satırı aç/kapat: boyut, güncelleme, oluşturma, senkron, kaynak (bilinmiyorsa "bilinmiyor"); birden çok satır açık kalabilir |
| `v` `o` | Sağ sütun (boyut + tarih, boyut, güncelleme, oluşturma, senkron, kaynak, yok) · sıralama menüsü (`n` ad, `u` güncelleme, `c` oluşturma, `s` boyut, `r` ters); ikisi de `tui-state.toml`'da kalır |
| `<` `>` `z` | Panel genişliği, yakınlaştırma · Fare: bölücü, tekerlek, tıklama; REVIEW'da sürükleme metni seçer ve bırakınca kopyalar (`Shift`+sürükle terminalin seçimi) |
| `,` `F9` / `F1` `?` | Ayarlar (`F9` her yerde, editörde kaydedilmemişi sorar) / yardım |
| `Esc` | Listede süzgeci temizler, çıkmaz |
| `q` | Liste ve önizlemeden çık (editörde ve girdilerde metindir) · `Ctrl-C` editör dışında acil çıkış (editörde kopyalar) |

EDIT'te `Ctrl+C` `Ctrl+X` `Ctrl+V` sistem panosunu kullanır: Wayland'de `wl-copy`/`wl-paste`, X11'de `xclip` ya da `xsel`, macOS'ta `pbcopy`/`pbpaste`, Windows'ta Win32. Araç yoksa kopya OSC 52 ile terminale gider (en çok 75 KB, üstünde ileti) ve yapıştırma için `Ctrl+Shift+V` önerilir; `notes` kendi iç panosunu yedek tutar.

EDIT'te satır sonunda soluk öneri çıkar (önce bu nottan, sonra diğer notlardan): `Tab` ya da `→` alır, yazmaya devam etmek düşürür; öneri yokken `Tab` sekme ekler. `Ctrl+Space` fzf gibi süzülen tamamlama listesini açar. `.env`, `properties`, `ini`, `toml`, `yaml`, `sh`, `json` ve benzeri notlardan yalnız anahtarlar önerilir; öbür notlarda da `KEY=değer` satırı yalnız anahtarını verir, değerler başka nota sızmaz. `md` ve `txt`'de PATH'te `hunspell` varsa yanlış sözcüklerin altı çizilir (sözlük dilden: `tr_TR`, `en_US`; içerik makineden çıkmaz); yoksa sessizce kapalıdır.

Ad verirken yazdığın uzantı değiştirilmez, yerine başkası konmaz, üstüne eklenmez (`a`, `n`, `F2`, `r`, `note -n`, GUI): `deneme.cpp`, `deneme.xyz`, `a.tar.gz`, `notlar.v2` aynen kalır; uzantı yazmadıysan `a` `default_type`, `n` ve `note -n` içerikten algılananı kullanır. `a` ve `n` gövdeyi slug'lar (`Toplantı Notu.CPP` → `toplanti-notu.CPP`); `F2`, `r` ve `note -n` yazdığını aynen alır.

En küçük pencere 60×15. Odaklı panelin çerçevesi kipe göre renklidir ve etiket taşır: `LIST` camgöbeği, `REVIEW` yeşil, `EDIT` eflatun, kaydedilmemişse `EDIT *`.

EDIT'te satır numarası sütunu (geçerli satır vurgulu), gerçek terminal imleci, geçerli satır arka planı, dosya sonunun ötesinde `~` ve durum çubuğu (dosya, tip, UTF-8, LF/CRLF, BOM, satır:sütun, toplam satır) vardır; tıklama imleci oraya koyar. Sözdizimi vurgusu REVIEW ve EDIT'te, uzantıya göre (kaydedilmemiş taslakta canlı algılamayla); 2 MiB'den büyük dosya ve 64 KiB'den uzun satır düz metindir.

### `notes-gui`

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (`↑↓` listede gezer, `Enter` açar) |
| `Ctrl+N` / `Ctrl+S` | Yeni not / şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu |
| `Ctrl+M` | Klasöre taşı |
| `Del` (listede), araç çubuğu Sil | Çöpe at, sormaz; durum çubuğunda Geri al |
| `Shift+Del` (listede) | Kalıcı sil (sorar, varsayılan Vazgeç) |
| Araç çubuğu `Çöp (n)` | Liste panelinde çöp: Geri getir, Kalıcı sil, Boşalt (sorar) |
| `Ctrl+E` | PDF dışa aktar |
| `F5` | Şimdi senkronla |
| `Ctrl+,` | Ayarlar sekmesi |

Mevcut notlar yazmayı bıraktıktan 3 sn sonra otomatik kaydedilir (`tuning.autosave_ms`, her kayıt günlüğe gövde kadar yer tutar; kapanış ve not geçişi hemen kaydeder). Dış değişiklikler `rescan_seconds` aralığında ve
pencereye dönünce algılanır. `Ctrl+E` bir yol alanı açar (elle yaz ya da Browse); başsız/betik PDF için `note export --pdf`.
Sınırlar: ekran okuyucu desteği yok; yalnız gömülü DejaVu fontu var (CJK ve renkli emoji `?` kutusu çıkar, dosya bozulmaz);
PDF yalnız soldan sağa yazar (Arapça/İbranice yok). Pencere yerleşimi `gui-state.toml`'da tutulur (durum dizini).

## Eski sürümden geçiş

Eski sürüm notları `notes_dir` altında düz dosya tutuyordu. Yeni sürüm bu dosyaları görünce hiçbir şey kaydetmez ve şunu ister:

```sh
note graph migrate
```

Oluşturulma zamanı dosyanın doğum zamanından alınır (yoksa son değişiklik). Önce özel bir yedek alır (`<veri dizini>/migration-backup-*`, yer ve silme komutu çıktıda), notları ve çöpü bayt bayt içe aktarır, doğrular. Eski dosyalara dokunmaz, kendiliğinden silmez; tekrar çalıştırmak güvenlidir. 60 MiB üstü dosya adıyla raporlanır ve yerinde kalır, geçiş sürer. Başka bir nedenle geçemeyen dosya (okunamayan, geçersiz ad) listelenir, kapı kapalı kalır; `note graph migrate --skip-failed` onları da yerinde bırakıp kapıyı açar.

Senkronlu iki cihazda: A'da `note graph migrate`, `note sync`; B'de `note graph migrate`, `note sync`. Sıra önemli değildir: aynı baytlar tek not olur, farklıysa kopya kalır. `note export --all DİZİN` ile denetledikten sonra eski dosyaları tek cihazda kaldır: `git -C <notes_dir> rm -r -- <dosyalar>`, `git commit`, `note sync`. Senkronsuz kullanımda dosyaları kendin silersin.

## Yapılandırma

| | Windows | Linux |
|---|---|---|
| `config.toml` | `%APPDATA%\noteapp\` | `$XDG_CONFIG_HOME/noteapp/` (`~/.config/noteapp/`) |
| log + senkron durumu | `%LOCALAPPDATA%\noteapp\` | `$XDG_STATE_HOME/noteapp/` |
| notlar (`graph.db`) | `%LOCALAPPDATA%\noteapp\` | `$XDG_DATA_HOME/noteapp/` (`~/.local/share/noteapp/`) |
| senkron deposu ve eski notlar (`notes_dir`) | `%USERPROFILE%\notes` | `~/notes` |

`graph.db` asıl veridir: senkron kapalıysa yedeğini sen alırsın (`note export --all DİZİN` ya da dosyanın kendisi).
`NOTEAPP_HOME` ayarlıysa config `$NOTEAPP_HOME/config`, durum `$NOTEAPP_HOME/state`, veri `$NOTEAPP_HOME/data`, varsayılan
`notes_dir` `$NOTEAPP_HOME/notes` altına gider (deneme/test için izole ortam); `config.toml`daki açık `notes_dir` değeri olduğu gibi
kalır. Ayarlar `note config`, TUI'de `,` ya da `F9`, GUI'de Ayarlar sekmesinden değişir; dosyayı elle de düzenleyebilirsin.

| Anahtar | Anlamı |
|---|---|
| `language` | `auto` / `tr` / `en` |
| `notes_dir` | Ayarlarda "Senkron ve geçiş klasörü": senkron deposu ve eski notların kaynağı; notlar burada durmaz, `graph.db`'dedir (`~` ve ortam değişkenleri genişler). Değiştirmek not listesini değiştirmez |
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
3. Önce token'ı kasaya yaz, sonra remote'u bağla (`init` private repoya erişmek için token'a ihtiyaç duyar):

```bash
note sync login github.com           # token'ı gizli ister (ekrana yansımaz); Windows Credential Manager'a gider
note sync init https://github.com/<kullanici>/notes.git
```

   **SSH alternatifi:** anahtarını GitHub hesabına ekle, bir kez `ssh -T git@github.com` çalıştırıp host anahtarını
   kabul et, sonra `note sync init git@github.com:<kullanici>/notes.git`. Uygulama anahtar saklamaz; `ssh`, agent ve
   `~/.ssh/config` kullanılır.
4. `note sync status` açık/kapalı, remote, son başarı ve son hatayı gösterir; `note sync` elle tek döngü çalıştırır.

Başlangıçta `notes_dir` boşsa repo klonlanır ve bu cihazın notları diğerleriyle birleşir; remote boşsa bu cihazın günlüğü ilk
commit olarak gider. `notes_dir`'de görünür dosya varken remote da doluysa `sync.both_non_empty` ile reddedilir.
Git yalnız taşıyıcıdır: depoya yalnız `graph/<cihaz>.oplog`, `.gitattributes` ve `.gitignore` commit'lenir. **Sınır:** günlük
sıkıştırılmaz; silinen bir notun baytları eski günlük satırlarında ve git geçmişinde kalır. Depo özel olmalı, şifreleme yoktur.

Sonrasında not ekleyen/değiştiren her komut arka planda senkronu başlatır; TUI/GUI açılışta, değişikliklerden sonra
(`idle_seconds`) ve çıkışta senkronlar. **Çakışma** (aynı not iki cihazda değişti): bir sürüm yerinde kalır, diğeri
yanına `ad (conflict YYYY-MM-DD HHMM).md` olarak yazılır (saat UTC), iki cihazda da aynı; hiçbir şey kaybolmaz. Farklı satırlardaki düzenlemeler otomatik birleşir, kopya yalnız aynı satır ya da ikili içerikte kalır; kopyayı elle birleştirip silersin.

Kapatmak: `note sync off` (repo ve geçmiş olduğu gibi kalır); token'ı silmek için `note sync logout`.

## Sorun giderme

- **PowerShell 5.1'de Türkçe metin boruya girince bozuluyor:** oturumda
  `$OutputEncoding = [Text.Encoding]::UTF8` çalıştır; mümkünse Windows Terminal kullan.
- **`sync.ssh_host_unknown`:** host anahtarı henüz bilinmiyor; bir kez `ssh -T git@github.com` çalıştırıp onayla.
- **Linux'ta `sync.keychain_unavailable` (Secret Service yok):** token düz dosyaya yazılmaz; ortama ver:
  `export NOTEAPP_GIT_TOKEN=<token>`. Bu token yalnız `NOTEAPP_GIT_TOKEN_HOST` adresine (varsayılan `github.com`)
  gönderilir; başka bir sunucu için bu değişkeni de ayarlayın.
- **Senkron kilitli (`sync.busy`):** başka bir senkron sürüyor; bekle ya da tekrar dene. Ayrıntı için log
  (`…/noteapp/log.txt`).

## Geliştirme

```bash
ctest --preset linux-gcc-debug --output-on-failure         # Windows'ta: windows-mingw-debug
ctest --preset linux-gcc-debug -R "core"                   # tek modül
scripts/check-fedora.sh                                    # Fedora imajında derle + test (docker; çalışmıyorsa podman)
clang-format -i $(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp')
```

Spec'ler: [SPEC.md](SPEC.md) (harita, teknoloji, sözleşmeler) ve modül spec'leri
[core](SPEC-core.md) · [detect](SPEC-detect.md) · [search](SPEC-search.md) · [sync](SPEC-sync.md) ·
[cli](SPEC-cli.md) · [tui](SPEC-tui.md) · [gui](SPEC-gui.md). Plan: [tasks/plan.md](tasks/plan.md),
görevler: [tasks/todo.md](tasks/todo.md).
