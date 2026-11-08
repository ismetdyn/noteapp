# noteapp

Terminalde yaşayan biri için hızlı, sunucusuz, sahibine ait bir not sistemi. Notlar veri dizininde tek bir gömülü
veritabanındadır (`graph.db`); baytlar aynen durur, `note export --all` hepsini düz dosya olarak geri verir. Üç ön yüz aynı notları ve aynı
ayar dosyasını paylaşır: `note` (CLI), `notes` (TUI), `notes-gui` (SDL3 + Dear ImGui masaüstü). İstenirse notlar private bir
GitHub reposuyla senkron kalır (Windows ↔ Linux).

## Özellikler

- `note "metin"` ile saniyesinde ekleme; stdin'den ya da sevdiğin editörden (`-e`) yazma
- İçerikten tip önerisi (md, java, py, json…); notun tipi dosya uzantısıdır
- fzf tarzı bulanık arama (Türkçe katlama dahil) ve tam metin içerik araması
- `#etiket`ler metinden: `#proje`, iç içe `#proje/alt`, Türkçe `#çalışma`; `tag:proje` ile süz, `note tags` ile say
- Arama ilişkileri de tarar: etiketi, ilişki adı ya da bağlı notunun adı eşleşen not ad ve içerik sonuçlarının altında nedeniyle gelir
  (`bağlı: bütçe.md (kullanır)`); `rel:kullanır`, `linked:bütçe` ile süz. Bağlı notun kaç adım uzakta aranacağı
  `limits.search.relation_depth` (varsayılan 1, `0` sınırsız); `[tuning] search_relations = false` ilişki satırlarını kapatır
- Kayıtlı Cypher görünümleri: `note view save iş "MATCH …"`, `view:iş` ile her aramada süz; salt okunur, notlar gibi senkronlanır
- `notes`: iki panelli TUI (liste + önizleme), nvALT mantığı: eşleşme yoksa Enter yeni not
- `notes-gui`: klasör ve notlar tek ağaçta (ya da Dolphin gibi Ayrıntılar tablosu) + editör, tek araç çubuğu, otomatik kayıt, ayarlar sekmesi, PDF dışa aktarma
- Veri kaybı yok: atomik yazma, dışarıdan değişiklik tespiti, onaysız silme yok
- Aç/kapa GitHub senkronu (libgit2; HTTPS+token ya da SSH); çakışmada iki kopya, force push yok
- Türkçe/İngilizce arayüz, tek `config.toml`; sırlar yalnız OS kasasında

## Kaynaktan kurulum

Gerekenler: C++23 derleyici, CMake ≥ 3.28, Ninja, `patch` (FTXUI yaması). FTXUI, libgit2, toml++, SDL3, Dear ImGui, libharu, md4c, doctest
vb. derleme sırasında otomatik indirilir (`cmake/Dependencies.cmake`); ağ erişimi gerekir. Fontlar (DejaVu; `notes-gui` için ayrıca Inter ve JetBrains Mono, OFL-1.1) depoda gömülüdür; yeniden çıkarma: `scripts/fetch-ui-fonts.sh`.

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
sudo dnf install gcc-c++ cmake ninja-build patch git libsecret-devel openssl-devel pkgconf-pkg-config \
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

**Günlük kullanım için yerel kurulum önerilir:** AppImage her açılışta FUSE bağlaması kurar, `notes` paketten açılınca
çok daha hızlı başlar. Betik yalnız `~/.local/share/noteapp/<sürüm>` ve `~/.local/bin` altına yazar, tekrar çalıştırılabilir;
`cmake --install` ağacını da kabul eder:

```bash
bash scripts/install-local.sh out/noteapp-*-x86_64.AppImage
```

Betik `~/.local/bin` içinde yalnız kendi kurduğu bağları (hedefi `~/.local/share/noteapp/` altında) değiştirir; başka bir dosya ya da bağ varsa dokunmaz, adını söyleyip durur. Yukarıdaki AppImage tarifinin bağları da yabancıdır: önce `rm ~/.local/bin/{note,notes,notes-gui}`. Paket önce geçici dizine açılır, sonra rename ile yerine geçer; hata önceki kurulumu bozmaz.

Eski sürüm dizinleri kendiliğinden silinmez, gerekmiyorsa kaldır: `rm -r ~/.local/share/noteapp/<sürüm>`. Tümünü kaldırmak için `bash scripts/install-local.sh --uninstall`: yalnız betiğin bağlarını ve sürüm dizinlerini siler. **Notlar aynı dizindedir (`graph.db`) ve silinmez;** uygulama verisini silmek ayrı ve bilinçli bir iştir.

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
note ls [SORGU] [-f KLASÖR]                # listele (sorgu: bulanık sıra, yoksa en yeni önce); argüman alır: note ls "plan tag:proje type:md"
note ls -l | note ls --json | note ls -0   # sütunlu (zaman, boyut, tür, yol) / JSON / NUL ayırıcılı; --limit N (-n N); terminalde, -l ve --json ile ilişkiyle bulunan not da nedeniyle
note tags [--json]                         # #etiketler ve not sayıları
note relation add a.md b.md --name kullanır --directed   # adlı, yönlü ilişki (varsayılan adsız, yönsüz)
note relation ls [NOT] [--json]            # ilişki adları ve sayıları / notun ilişkileri (yol, ad, yön, kimlik)
note relation rename a.md b.md ESKİ YENİ   # adı değiştir ("" = adsız); rm a.md b.md [--name AD | --id KİMLİK]
note prop set a.md durum bitti             # nota özellik yaz (tür değerden: 3 sayı, true bool; --type text|number|bool)
note prop get a.md [durum] [--json]         # özellikler (uygulamanınkiler read-only) ya da tek değer; rm a.md durum siler
note relation prop set KİMLİK ağırlık 2     # ilişkinin özelliği (kimlik: note relation ls NOT); get / rm aynı
note ls prop:durum=bitti                    # özelliğe göre süz (prop:durum: anahtarı olan her not); eşleme
                                            # metinseldir: prop:fiyat=2.50, saklı 2.5'i bulmaz
note import rapor.txt                      # dosyayı bayt bayt not yap: rapor.txt (ad doluysa rapor-2.txt)
note import ~/belgeler --folder arsiv      # klasörü alt klasörleriyle: arsiv/belgeler/...; bağ ve gizli girdi gelmez
note import x.log --name kayit.md --overwrite   # tek dosyaya ad ver, aynı adlı notun üzerine yaz; --json, --type TİP
note view save AD 'CYPHER' [--force]       # salt okunur sorguyu adla kaydet; view ls|run AD|show AD|rm AD, --json
note find bütçe onay                       # içerik araması → yol:satır: kesit; terminalde, -l ve --json ile ilişkiyle bulunan not da nedeniyle; --limit N; ls gibi argüman alır (tag:, type:…); -n süzgeçten sonra sayar, ilişki satırları dahil
note show SORGU | note edit SORGU          # en iyi eşleşmeyi yazdır / editörde aç
note mv KAYNAK HEDEF                       # taşı/yeniden adlandır; uzantı değişimi = tip değişimi
note rm YOL [-y]                           # yalnız tam yol; TTY yoksa -y (--yes) şart
note export --pdf YOL [-o ÇIKTI] [--force] # PDF (A4; GUI ve ekran gerekmez); var olan dosya --force olmadan ezilmez
note export --all DİZİN                    # tüm notlar bayt bayt düz dosya, çöp .trash/ altında; üzerine yazmaz, bağdan geçmez; ulaşılamayan ya da yazılamayan not varsa çıkış 1
note graph export --format jsonl [-o DOSYA] | note graph import [--overwrite] DOSYA   # tüm graph'ı taşı
note sync [status | on | off | init REMOTE [--replace] | login [HOST] | logout [HOST]]
note config path | list | get ANAHTAR | set ANAHTAR DEĞER
note completion zsh | bash | fish          # kabuk tamamlama betiği (aşağıda)
note --help | note help KOMUT              # gruplu komut listesi / tek komutun ayrıntısı (örneklerle)
note ls --help                             # her komutta -h/--help: note help ls ile aynı
note -- ls                                 # "ls" metnini not et (alt komut adıyla çakışma); note -a ls de olur
```

Komutların kısa bayrakları TUI tuşlarıyla aynıdır. Bayrak **yalnız ilk argüman** olarak komuttur, kalan argümanlar o
komuta aynen gider (`note -i a.txt b.md --folder x` = `note import a.txt b.md --folder x`, `note -s sözcük -n 5` =
`note find sözcük -n 5`):

| Kısa | Uzun | Anlam |
|---|---|---|
| `-a METİN…` | `--add` | düz ekleme (`note METİN…` ile aynı; `note -a ls` "ls" metnini ekler) |
| `-n AD METİN…` | `--name` | adla ekleme |
| `-l` | `--ls` | `note ls` |
| `-s` | `--find` | `note find` |
| `-S` | `--show` | `note show` (`note cat` da aynı) |
| `-E` | `--edit` | `note edit` (`-e` editör seçeneğidir) |
| `-d` | `--rm` | `note rm` |
| `-m` | `--mv` | `note mv` |
| `-x` | `--export` | `note export` |
| `-T` | `--tags` | `note tags` |
| `-i` | `--import` | `note import` |

`-t` tip, `-f` klasör, `-e` editör, `-q` sessiz, `-h` yardım olarak kalır. Değer alan seçeneğin ardındaki sözcük değerdir:
`note -n -i` "-i" adlı not ekler. Tireyle başlayan metin için `note -- -i`.

Boru ve yönlendirme kabuğundur, `note` stdin/stdout ile çalışır:

```bash
note -n deneme < dosya.txt                 # dosyanın içeriği "deneme" adlı not olur
cat dosya.txt | note -n deneme             # aynısı, borudan
note -S deneme | grep x                    # notu göster, yalnız x geçen satırlar
note -S deneme >> cikti.txt                # notu dosyanın sonuna ekle (> üzerine yazar)
note -n deneme <<'EOF'                     # << dosya değil heredoc: sonraki satırlar EOF'a kadar stdin olur
ilk satır
EOF
```

`--no-sync` yazan komutlarda arka plan senkronunu atlar. Her kısa bayrağın uzun adı var (`-t/--type`, `-f/--folder`,
`-n/--name`, `-y/--yes`, `-o/--output`...); değer `--type=md` biçiminde de verilebilir. `-q/--quiet` başarı satırını
(`Kaydedildi:`, `Silindi:`...) yazmaz, hatalar yine görünür. Çıkış kodları: `0` başarı · `1` kullanıcı kaynaklı (iptal,
bulunamadı) · `2` kullanım hatası · `3` G/Ç/iç hata · `4` senkron hatası.

Hatalı kullanımda ilk satırın altında ipucu çıkar (`ipucu: bunu mu demek istediniz: 'status'?`, `ipucu: ayrıntı için
'note help sync'`). Komut adına benzeyen tek sözcük (`note lss`) yine not olarak kaydedilir; terminalde geri alma
ipucu gösterilir.

Terminal: renk `NO_COLOR`, `TERM=dumb` ve `NOTEAPP_COLOR=always|never|auto` ile yönetilir; boruya renk gitmez. Ekrana
sığmayan `ls`, `find`, `show`, `tags`, `config list`, `view ls` ve yardım çıktısı `NOTEAPP_PAGER`, yoksa `PAGER`, yoksa
`less -FIRX` ile sayfalanır (kabuksuz: `PAGER="less -R"` gibi argümanlar tırnak kuralıyla bölünür, program yalnız PATH'in
mutlak girdilerinde aranır); `--no-pager` ya da `NOTEAPP_PAGER=cat` kapatır, boruda pager hiç açılmaz (Windows'ta
pager yok). `note sync` ve `note import` terminalde çalışırken tek satırlık ilerleme yazar (içe almada `n/m`).

#### Kabuk tamamlama

`note completion zsh|bash|fish` tamamlama betiğini stdout'a yazar, dosyaya kendisi yazmaz. Tab komutları, fiilleri
(`sync status`, `view run`...), seçenekleri, not yollarını, klasörleri, tipleri, etiketleri, görünümleri, config
anahtarlarını ve `ls`/`find` sorgusunda `tag:`, `type:`, `in:`, `view:`... argümanlarını önerir. zsh ve fish menüsünde
(fzf-tab dahil) açıklamalar arayüz dilindedir. Dinamik değerleri gizli `note __complete` verir: yalnız okur, depo yoksa
boş döner. Betik `note` sürümüyle değişir; güncellemeden sonra yeniden üret.

zsh (fzf-tab ve zsh-autosuggestions ile de çalışır):

```zsh
mkdir -p ~/.local/share/zsh/site-functions
note completion zsh > ~/.local/share/zsh/site-functions/_note
# ~/.zshrc içinde compinit'ten (oh-my-zsh'te `source $ZSH/oh-my-zsh.sh`ten) ÖNCE:
fpath=(~/.local/share/zsh/site-functions $fpath)
```

Sonra `rm -f ~/.zcompdump*; exec zsh`. Yazılabilir bir fpath dizinin varsa `note completion zsh > "${fpath[1]}/_note"` de olur.

bash (`~/.bashrc`):

```bash
source <(note completion bash)
```

fish:

```fish
note completion fish > ~/.config/fish/completions/note.fish
```

`scripts/install-local.sh` zsh ve bash dosyalarını kendisi yazar: `~/.local/share/zsh/site-functions/_note` ve
`~/.local/share/bash-completion/completions/note` (`XDG_DATA_HOME` / `BASH_COMPLETION_USER_DIR` ayarlıysa onların
altına). bash-completion paketi bash'te dosyayı kendiliğinden bulur; zsh'te yukarıdaki `fpath` satırı yine gerekir.
Betik yalnız kendi ürettiği dosyanın üzerine yazar, `--uninstall` onları siler.

### `notes` (TUI)

| Tuş | Eylem |
|---|---|
| `:` | Komut paleti (liste, REVIEW, çöp): her komutu adıyla süz ve çalıştır, ör. `move — Taşı  m`. `↑↓`, `Enter`, `Esc`. Sağda tuşu, son kullanılanlar başta; o an çalışamayan soluk ve nedenli. Tuşu olmayanlar da burada: sütun seçimi, tüm klasörleri aç/kapat, dosya ikonları. Tuş ve palet aynı tablodandır |
| `↑` `↓` `j` `k` | Listede gezin, sağ panel canlı izler. Liste klasör tabanlıdır: önce klasörler, sonra notlar, numara klasör başına; üstte yol (`notlar/work/sub`, parçaya tıkla). Seçim satır vurgusudur |
| `Space` | Klasörü yerinde aç/kapat (`▸` `▾`, oka tıkla): alt öğeler girintili, her derinlikte, numara kendi klasöründe; açık olanlar `tui-state.toml`'da kalır. Notta ayrıntıyı açar |
| `m` | Seçimi ya da satırı klasöre taşı: altta girdi, geçerli klasörle dolu, `Tab` klasörleri önerir; yazılan yeni iç içe klasör oluşur. Ezme yok: hedef doluysa hata girdide kalır |
| `L` | Notu başka nota ilişkilendir: altta girdi, `Tab` not yollarını önerir (liste ve REVIEW); hedef `[[bağlantı]]` gibi çözülür (tam yol, uzantısız yol ya da yalın ad; GUI ile aynı); `Enter`dan sonra ilişkinin **adı** sorulur: `Tab` daha önce girilmiş adları tamamlar, `Ctrl+D` yönlü yapar (`—` yönsüz, `→` yönlü; bilinen adda o adın alışkanlığı gelir), boş `Enter` adsız. Bağlantı bölümünde ilişkili satırda (`↗ b.md — kullanır`) ilişkiyi kaldırır (sorar, varsayılan Vazgeç); `n` adını değiştirir, `f` yönünü çevirir, `u` yönsüz yapar |
| `b` | REVIEW'da önizleme altındaki bağlantı bölümüne geçer: `Gelen`, `Giden`, `İlişkili`, `Eksik`; `↑↓` seç, `Enter` ya da tıklama nota gider, `Esc` `b` döner |
| `←` `h` | Üst klasör, seçim çıkılan klasörde kalır; kökte etkisiz; arama sonucunda notun klasörüne gider |
| `→` `l` | Klasörde içeri girer (`Enter` ve çift tıklama da); notta önizleme (REVIEW, `cat` gibi): `↑↓` `PgUp/PgDn` `g` `G` kaydır, `/` notta bul, `n` `N` gez, `←` `Esc` liste, `q` çık. `md`, `csv`, `json` biçimli açılır, `R` ham metne geçer ve geri döner |
| `Enter` `e` | Düzenle (EDIT): `Ctrl+S` kaydet, `Ctrl+Q` çık (kaydedilmemişi sorar), `Ctrl+F` bul, `Ctrl+Z/Y`, `Ctrl+X/C/V`; `Esc` yalnız aramayı kapatır ya da seçimi kaldırır, tek başına çıkmaz |
| `E` | Harici editörde aç (`$EDITOR`) |
| `/` | Alt satırda ara: `↑↓` seç, `Enter` seçimi tut (eşleşme yoksa taslak), `Ctrl+T` ad/içerik, `Esc` kapat |
| `s` | Ad ve içerikte ara (eski `f` de çalışır): eşleşen satır yolun altında, `Enter` REVIEW'da eşleşmeye kayar. `/` ve `s` argüman alır: `sort:-size` `type:md` `in:work/` `size:>1mb` `updated:7d` `sync:modified` `tag:proje` `view:iş` `rel:kullanır` `linked:bütçe`; bozuk değer metin sayılmaz, yeriyle hata gösterir. Ad ya da içerikte bulunmayan ama etiketi, ilişki adı ya da bağlı notunun adı eşleşen not en altta, nedeni satırın sonunda soluk |
| `a` `Ctrl-O` | Yeni not: tüm terminali alan boş editör. Ad ilk `Ctrl+S`'te altta sorulur (`klasör/ad.uzantı`, önerisi ilk satır + algılanan tip), o ana dek dosya yok; `Ctrl+Q` iki panele döner |
| `n` | Yeni not ya da klasör, ad bulunulan klasöre göre: `ad/` klasör oluşturur ve Listede kalır, `sub/x.md` gereken klasörleri açar. Listenin başında satır açılır, `Enter` boş notu oluşturup EDIT'te açar (yazmaya ya da yapıştırmaya hazır), `Esc` vazgeçer; uzantı yoksa `default_type`; dolu ya da geçersiz ad satırda hata verir · `r` taşı/ad/tip · `Ctrl+S` senkron (REVIEW'da da) |
| `d` `u` `D` | `d` çöpe atar (sormaz), `u` bu oturumda son atılanı geri alır, `D` kalıcı siler (sorar, varsayılan Vazgeç). Klasörde: `d` notları tek tek çöpe atıp klasörü kaldırır (notlu klasörde sorar, varsayılan Vazgeç), `u` hepsini geri getirir; `D` sayıları gösterip sorar, kalıcıdır |
| `F2` | Yol ve ad: altta tek satır, bulunulan klasöre göre; `/` içeren yol taşır; klasörde (`r` de) klasörü yeniden adlandırır; imleç adın sonunda; `Enter` taşır (hedef varsa hata, ezme yok), `Esc` vazgeçer |
| `t` | Çöp görünümü (ya da üstteki `çöp: N`e tıkla; haritada `gt`): `u` geri getir, `D` kalıcı sil, `E` tümünü boşalt (ikisi de sorar, varsayılan Vazgeç), `q` `Esc` geri; sağda not salt okunur |
| `c` | Notun özgün baytlarını sistem panosuna kopyalar (liste ve REVIEW); ikili ya da UTF-8 olmayan notta nedenini söyler |
| `Alt+→` / `I` | Seçili satırın (klasör de) / tüm satırların ayrıntısını aç-kapat (çöpte `i` de) |
| `i` `x` `T` `S` | İçe al (yol girdisi) · seçili notu PDF'e aktar · etikete göre ara · önizle (`→` gibi) |
| `v` `X` `Ctrl+A` | Satırı seç ve aşağı in, seçimi temizle, görünenleri seç; seçim klasör değişince kalır, altta `seçili: 3` |
| `y` `p` | Seçimi ya da satırı kopya için işaretle, geçerli klasöre yapıştır. Hiçbir şey ezilmez: dolu ad `-2` alır, raporlanır. `Esc` vazgeçer |
| `:` `import` | Dosya ya da klasör içe al (`note import` kuralı): altta yol girdisi (göreli yol çalışma dizinine göre), hedef `n` ile aynı klasör; arka planda koşar, ilerleme (`n/m` ve yol) alt satırda, `Esc` ya da `Ctrl+C` durdurur (alınanlar kalır); özet alt satırda, ilk not REVIEW'da açılır; hiçbir şey alınamazsa girdi nedeniyle geri açılır. Komut satırından: `notes import YOL... [--folder K] [--name AD] [--overwrite]` önce özeti yazar, sonra TUI'yi ilk notla açar (hiçbir şey alınamazsa açmaz, çıkış 1); `notes --help` kısa kullanım |
| `gg` `gh` `gt` | Baş, kök klasör, çöp (`g` sıradaki tuşu bekler) |
| `PgUp` `PgDn` `gg` `G` | Listede sayfa sayfa, baş, son; REVIEW'da `↑↓` basılı tutunca hızlanır, `Ctrl+U` `Ctrl+D` yarım sayfa, `J` `K` 5 satır |
| `Alt+→` `Alt+←` | Satırı aç/kapat: boyut, güncelleme, oluşturma, senkron, kaynak (bilinmiyorsa "bilinmiyor"); birden çok satır açık kalabilir |
| `V` `o` | Sağ sütun (boyut + tarih, boyut, güncelleme, oluşturma, senkron, kaynak, yok) · sıralama menüsü (`n` ad, `u` güncelleme, `c` oluşturma, `s` boyut, `r` ters); ikisi de `tui-state.toml`'da kalır |
| `<` `>` `z` | Panel genişliği (`tui-state.toml`'da kalır), yakınlaştırma · Fare: bölücü, tekerlek, tıklama; REVIEW'da sürükleme metni seçer ve bırakınca kopyalar (biçimli görünümde görüneni; `c` her zaman özgün baytları; `Shift`+sürükle terminalin seçimi) |
| `,` `F9` / `F1` `?` | Ayarlar (`F9` her yerde, editörde kaydedilmemişi sorar) / yardım |
| `Esc` | Listede süzgeci temizler, çıkmaz |
| `q` | Liste ve önizlemeden çık (editörde ve girdilerde metindir) · `Ctrl-C` editör dışında acil çıkış (editörde kopyalar) |

Bağlantılar metindeki `[[hedef]]`ten türer. Hedef yeniden adlandırılınca not metni **yeniden yazılmaz**: eski ad `Eksik` listesinde görünür, bağlantıyı metinde sen düzeltirsin. `L` ile kurulan ilişki metne bağlı değildir, ad değişiminde kalır.

Etiketler `md`, `markdown`, `txt` ve uzantısız notlarda metindeki `#etiket`ten türer (harf, rakam, `_`, `-`, `/`; en az bir harf). Başlık (`# Başlık`), kod bloğu, satır içi kod, URL parçası (`sayfa#bolum`) ve `[[not#bölüm]]` etiket değildir; kodda `#include` de değil. Büyük/küçük harf eşleşmede fark etmez, ilk yazılış gösterilir. Notun etiketleri önizlemenin altındaki ayırıcı satırında (`#iş #proje/alt ───`) ve GUI başlığında görünür. `tag:proje` `proje` ve `proje/alt`'ı bulur, `tag:proje/alt` yalnız o dalı; paletteki (`:`) "Etikete göre süz" aramayı `tag:` ile açar ve etiketleri listeler. Çöpteki not sayılmaz. Graf olarak `(:Note)-[:TAGGED]->(:Tag {name})`: `note query "MATCH (n:Note)-[:TAGGED]->(t:Tag {name:'proje'}) RETURN n.name"`. Önceki sürümden gelen notlar ilk açılışta kendiliğinden etiketlenir.

Görünüm, adla saklanmış bir Cypher sorgusudur: `note view save iş "MATCH (n:Note)-[:TAGGED]->(:Tag {name:'iş'}) RETURN n"`. `note view run iş` `note query` ile aynı tabloyu (`--json` ile JSON) verir; `note view ls`, `show`, `rm` yönetir, var olan adı `--force` olmadan ezmez. `view:iş` her aramada (`note ls`, TUI `/` `s`, GUI arama kutusu) listeyi görünümün döndürdüğü notlara süzer: görünüm not düğümünü (`RETURN n`) ya da not yolunu döndürmelidir. TUI paletindeki (`:`) "Görünüm çalıştır" aramayı `view:` ile açar ve görünümleri listeler. Görünüm TUI ve GUI'den de kaydedilir: haritada sorgu çalıştırıp TUI'de `K` (keep), GUI'de sorgu çubuğundaki `Görünüm kaydet` (ikisinde de palette "Sorguyu görünüm olarak kaydet") gösterilen okumayı adla kaydeder; ad kuralı CLI'nınkidir (baş/son boşluk ve `/` yok, var olan ad ezilmez). Paletteki "Kayıtlı görünümü sil" sorar: TUI'de ad girdisi (`Tab` adları önerir, ikinci `Enter` siler), GUI'de seçici ve onay penceresi. Görünümler notlar gibi senkronlanır, bu yüzden **salt okunurdur**: yazan sorgu (`CREATE`, `SET`, `DELETE`…) kaydedilmez, başka cihazdan gelse de çalıştırılmaz, hiçbir şey değiştirmez.

EDIT'te `Ctrl+C` `Ctrl+X` `Ctrl+V` sistem panosunu kullanır: Wayland'de `wl-copy`/`wl-paste`, X11'de `xclip` ya da `xsel`, macOS'ta `pbcopy`/`pbpaste`, Windows'ta Win32. Araç yoksa kopya OSC 52 ile terminale gider (en çok 75 KB, üstünde ileti) ve yapıştırma için `Ctrl+Shift+V` önerilir; `notes` kendi iç panosunu yedek tutar.

EDIT'te satır sonunda soluk öneri çıkar (önce bu nottan, sonra diğer notlardan): `Tab` ya da `→` alır, yazmaya devam etmek düşürür; öneri yokken `Tab` sekme ekler. `Ctrl+Space` fzf gibi süzülen tamamlama listesini açar. `.env`, `properties`, `ini`, `toml`, `yaml`, `sh`, `json` ve benzeri notlardan yalnız anahtarlar önerilir; öbür notlarda da `KEY=değer` satırı yalnız anahtarını verir, değerler başka nota sızmaz. `md` ve `txt`'de PATH'te `hunspell` varsa yanlış sözcüklerin altı çizilir (sözlük dilden: `tr_TR`, `en_US`; içerik makineden çıkmaz); yoksa sessizce kapalıdır.

Girdiler zsh gibi çalışır (`s`, `/`, `n`, `F2`, ad istemi, editörde bul): metnin sonunda soluk öneri çıkar, `→` alır; `Tab` tek adayı yazar, çoksa süzülen liste açar (`↑↓`, `Enter`, `Esc`): klasörler, not adları, `.cpp` gibi tipler; `f` ve `/`'de `sort:` `type:` `in:` `size:` `updated:` `created:` `origin:` `sync:` `tag:` `view:` `rel:` `linked:` anahtarları ve değerleri. Arama satırında geçerli argüman renkli, bozuk değer anında kırmızıdır. `Ctrl+P` `Ctrl+N` metni içeren önceki aramalarda gezer (`↑↓` sonuçları gezmeye devam eder); geçmiş `tui-state.toml`da yereldir, en çok 200 kayıt, senkrona girmez, sır benzeri değer ya da not adı taşıyan sorgu yazılmaz.

Ad verirken yazdığın uzantı değiştirilmez, yerine başkası konmaz, üstüne eklenmez (`a`, `n`, `F2`, `r`, `note -n`, GUI): `deneme.cpp`, `deneme.xyz`, `a.tar.gz`, `notlar.v2` aynen kalır; uzantı yazmadıysan `a` `default_type`, `n` ve `note -n` içerikten algılananı kullanır. `a` ve `n` gövdeyi slug'lar (`Toplantı Notu.CPP` → `toplanti-notu.CPP`); `F2`, `r` ve `note -n` yazdığını aynen alır.

En küçük pencere 60×15. Odaklı panelin çerçevesi kipe göre renklidir ve etiket taşır: `LIST` camgöbeği, `REVIEW` yeşil, `EDIT` eflatun, kaydedilmemişse `EDIT *`.

REVIEW'da `.md` `glow` gibi biçimli görünür: başlık, kalın, eğik, kod (dil etiketiyle renkli), alıntı, liste, görev listesi, sarılan tablo, bağlantı metin + soluk URL, görsel `[görsel: alt]`, HTML yazıldığı gibi; ağa ya da dosyaya gidilmez. `.csv` hizalı tablo, `.json` girintili görünür; büyük not da biçimli açılır (yalnız görünen kısım üretilir). Geçersizse ya da `limits.tui.view_bytes` ile bir sınır verilmiş ve not büyükse (varsayılan `0`, sınırsız) ham metin ve soluk neden. `R` görünüm/ham arasında geçer (etikette yazar, oturum boyunca kalır), EDIT her zaman ham metindir. `/` gösterilen metinde arar, `e` üstte görünen bloğun kaynak satırında açar. Ham görünümde ekrandan uzun satırın devamı `Shift+←/→` ya da `Shift`+tekerlekle yana kaydırılarak okunur.

EDIT'te satır numarası sütunu (geçerli satır vurgulu), gerçek terminal imleci, geçerli satır arka planı, dosya sonunun ötesinde `~` ve durum çubuğu (dosya, tip, UTF-8, LF/CRLF, BOM, satır:sütun, toplam satır) vardır; tıklama imleci oraya koyar. Sözdizimi vurgusu REVIEW ve EDIT'te, uzantıya göre (kaydedilmemiş taslakta canlı algılamayla); `limits.tui.highlight_bytes`'tan (2 MiB) büyük dosya ve `highlight_line_bytes`'tan (64 KiB) uzun satır düz metindir (`0` = sınırsız).

### `notes-gui`

Pencerenin üstünde tek araç çubuğu, dosya yöneticisindeki gibi gruplu: `Yeni not` `Yeni klasör` `İçe al` | `Yeniden adlandır` `Taşı` `Sil` | `Çöp (n)` `PDF'e aktar`, sağda görünüm seçici `Ağaç` / `Ayrıntılar` / `Harita` (seçim `gui-state.toml`'da kalır). Düğmeler seçili satıra uygulanır: klasör satırında klasörü yeniden adlandırır ya da (onayla) siler, not satırında notu; `Yeni not` ve `Yeni klasör` seçili klasörün, not seçiliyse notun klasörünün içine açar. Yapamayacağı işte düğme kapalıdır, ipucu nedenini ve kısayolu söyler.

- **Ağaç** (varsayılan): TUI'deki gibi klasörler ve notlar tek panelde, kök `Tümü`; oka ya da çift tıklayınca klasör açılır/kapanır. Açık klasörler `gui-state.toml`'da kalır; seçili not açılışta görünür olur.
- **Ayrıntılar**: yalnız geçerli klasörün içeriği, sütunlar Ad, Boyut, Değiştirilme, Oluşturulma; önce klasörler. Klasöre çift tıklama ya da `Enter` içine girer; üstte `Yukarı` ve tıklanabilir yol düğmeleri.
- **Harita**: notlar çizge olarak, editör sağda kalır. Kenarlar `[[bağlantı]]` (oklu) ve ilişki (renkli, oksuz); düğüm rengi not tipinden, gösterilen not halkalı. Üstte `Tüm çizge` (kapalıyken gösterilen notun çevresi), `Derinlik` (varsayılan 2, en çok `limits.app.map_depth` = 6; `0` sınırsız) ve `Etiketler` (etiketler küçük kare). Tıklama notu açar (bekleyen düzenleme önce kendi notuna kaydedilir), çift tıklama açıp Ağaç'a geçer, düğümü sürüklemek onu oraya sabitler. Tekerlek imlecin çevresinde yakınlaştırır; boşluğu sürüklemek ya da orta tuş kaydırır; harita odaktayken oklar kaydırır, `+`/`-` yakınlaştırır, `0` sığdırır. **İlişki kurma:** `Shift` basılıyken bir düğümü başka düğüme sürükle: not → not küçük menüde `İlişkili` ya da `Bağlantı` (ilk notun sonuna `[[yol]]` eklenir), not ↔ etiket etiketi nota ekler. `İlişkili` (ve `Ctrl+R`) ilişkinin **adını** sorar: daha önce girilmiş adlar kullanım sayılarıyla listelenir (yazdıkça süzülür, `↑↓` + `Enter` ya da tıklama), yeni ad yazılabilir, boş ad adsızdır; `Yönlü` kutusu ilk nottan ikinciye ok çizer (bilinen adda o adın alışkanlığı gelir). Kenara sağ tık: ilişkiyi yeniden adlandır, yönünü çevir / yönlü ya da yönsüz yap, ilişkiyi ya da etiketi kaldır (cümledeki etiketin yalnız `#`i gider, sözcük kalır); `[[bağlantı]]` notun metnindedir, editörde silinir. Yakınlaşınca ilişkinin adı kenarın ortasında yazılır, üzerine gelince ipucunda her zaman görünür. Bağlantı paneli ilişkiyi `↗ b.md — kullanır` diye gösterir (↗ çıkan, ↙ gelen), sağ tık aynı işlemleri sunar. Bağlantı ve etiket yalnız metin notlarına (md, txt, uzantısız) yazılır. TUI haritasında aynısı: `C` (Tab hedef, Enter, `r` ad satırı / `l`, `d` kaldır, ilişkide `n` ad, `f` yön çevir, `u` yönsüz, Esc), düğümden sağ sürükleme, kenara sağ tık + `d`. Arama kutusu eşleşen notları vurgular, gerisini soldurur (`tag:` dahil). **Sorgu çubuğu:** `Sorgu` çipi ya da haritada `Q` tek satırlık Cypher açar: `Enter` (ya da `Ctrl+Enter`) çalıştırır, `↑↓` geçmiş, `Görünümler` listesi kayıtlı görünümü çalıştırır (adını yazmak da). Sonuçtaki notlar ve kenarlar yanar, gerisi söner; `Tablo` sonucu tablo olarak gösterir (not hücresine tıklama notu açar). Veriyi değiştiren sorgu (`CREATE`, `SET`, `DELETE`, `MERGE`, `REMOVE`) önce kaç değişiklik yapacağını söyler ve `Uygula` ister (geri alma yok). Kendi düğümlerin, var olan ilişkinin adı, yönü ve silinmesi (`RELATED_TO`, `name`/`directed`) ve notların kendi özellikleri yazılır (`MATCH (n:Note) WHERE n.name = 'plan' SET n.status = 'done'`); not metni, yolu, `[[bağlantı]]` ve `#etiket` kenarları, klasörler, görünümler ve yeni ilişki sorguyla değişmez (hata nedenini ve yerini söyler) — onları uygulamanın kendisinden değiştir (ilişki: `Bağla…`, TUI `C`/`L`, `note relation add`). `note query --write` aynı kurala uyar. Uzun hücreler tabloda `limits.app.query_cell_bytes`'ta (4 KiB) kısalır. `Görünüm kaydet` gösterilen okumayı adla kayıtlı görünüm yapar. TUI haritasında aynısı: `Q` satırı (`Tab` görünüm adları), `O` tablo, `K` görünüm kaydet, `Esc` sonucu kaldırır. **Açılışta:** harita açıkken kapatırsan (TUI ve GUI) sonraki açılış haritayla, aynı derinlik ve süzgeçlerle başlar; çalıştırdığın ya da kaydettiğin son görünüm arka planda yeniden çalışır, sonuç gelince harita yanar (yalnız okur; silinmiş ya da yazan sorguya dönmüşse neden bir kez söylenir; geçici depo hatasında ad kalır, sonraki açılış yeniden dener). Sorgu çubuğunu kapatmak sonucu kaldırmaz; haritada `Esc` kaldırır (TUI'de de). Yazılan sorgunun metni saklanmaz: kalıcı olsun istiyorsan `K` / `Görünüm kaydet` ile görünüm yap. **Özellikler:** sağ bölmedeki `Özellikler` paneli (başlığa tıkla) gösterilen notun özelliklerini tablo olarak gösterir: üstteki satırdan anahtar ve değer yazıp `Ekle` (tür değerden anlaşılır ya da seçilir; `#ad` nota etiket ekler), satırda `Düzenle` ve `Sil` (onay sorar). Notun adı, tipi, tarihleri gibi uygulamanın alanları salt okunurdur. İlişkinin özellikleri kenar menüsünden ya da bağlantı panelinde ilişki satırına sağ tıkla `Özellikler…`; ilişkinin adı ve yönü yine o menülerden değişir. TUI'de `p` (REVIEW'da not, bağlantı bölümünde ilişki, haritada seçili not ya da `C` kipinde ilişki), `a` ekle, `e`/`Enter` düzenle, `d` sil, `Esc` geri; değer satırında `text:42` gibi önek türü seçer. Aramada `prop:durum=bitti`. En çok 2.000 düğüm çizilir, aşılınca üstte uyarı çıkar. Hedefinden önce kaydedilmiş bir notun `[[bağlantı]]`ı, not yeniden kaydedilene dek haritada çizilmez (bağlantı panelinde görünür).
- **İçe al** (araç çubuğu menüsü `Dosya içe al…` / `Klasör içe al…`, palette `import`, ya da dosya/klasörleri pencereye sürükleyip bırak): yerel diyalogda çoklu seçim; `note import` kuralıyla bayt bayt, `Yeni not`un klasörüne. Arka planda koşar: ilerleme durum çubuğunda, `İçe almayı durdur` keser (alınanlar kalır). Özet durum çubuğunda, ilk not sekme kuralıyla açılır: iğnelenmiş, tutulan ya da yazılmış taslak sekmesi yerinde kalır, not yeni sekmede; kaydedilmemiş düzenleme önce kaydedilir. Linux'ta diyalog XDG portal ya da `zenity` ister; yoksa durum çubuğu söyler, sürükle-bırak yine çalışır. `notes-gui --help` kısa kullanım.
- Klasör satırı seçilince açık notun bekleyen düzenlemesi önce kaydedilir, sağ panelde editör yerine klasör özeti görünür: yol, öğe / not / klasör sayısı, toplam boyut, son güncelleme ve alt ağacın en yeni notları (TUI ile aynı).
- Arama kutusuna yazınca liste düz sonuca döner (vurgulu, seçili klasörün içinde); çöp görünümü de düz listedir. Yanındaki düğme `Ad` / `İçerik` arasında geçer (`Ctrl+Shift+M`); kutuda `Tab` argümanı, etiketi ya da not adını tamamlar (çok aday varsa liste: yazı süzer, `Enter` ekler, `Esc` kapatır), soluk öneriyi `→` alır, `Ctrl+P` önceki aramalara döner (`Ctrl+N` ileri); argümanlar renklidir, bozuk değer kırmızı. Bulunan aramalar `gui-state.toml`'da hatırlanır (TUI kuralıyla, parola benzeri sorgu yazılmaz); `Ctrl+Shift+F` ad ve içerikte birlikte arar (`Tümü`), içerikte bulunan notun en iyi satırı yolun altında görünür. `Ctrl+Shift+T` etiketleri, `Ctrl+Shift+R` kayıtlı görünümleri listeler; seçilen `tag:` ya da `view:` sorgusu olur. Her aramada ilişkiyle bulunan notlar (etiket, ilişki adı, bağlı not) en altta, nedeni satırın sağında soluk. `sync:new` gibi senkron argümanı not başına durumu kullanır (senkron açıkken).
- **Sırala** düğmesi TUI'nin `o` menüsüdür: ad, güncelleme, oluşturma, boyut ya da ters çevir. Sıra ağaca, Ayrıntılar'a ve metinsiz aramaya uygulanır, varsayılan en yeni başta; seçim `gui-state.toml`'da kalır.
- **Sıra no:** Ayarlar → Görünüm → `Sıra no` Ağaç ve Ayrıntılar'da TUI'deki gibi satır numarası gösterir (her klasörde 1'den); hemen uygulanır, `gui-state.toml`'da kalır.
- **Bilgi paneli** (`Ctrl+I` ya da `Bilgi` başlığı): seçili notun boyutu, güncelleme ve oluşturma zamanı, senkron durumu, kaynak cihazı, tipi ve etiketleri; TUI'deki satır ayrıntısıyla aynı alanlar. Başlangıçta kapalıdır.
- **Önizleme** (`md` ve `markdown` notlarında, `Ctrl+Shift+V` ya da başlıktaki `Önizleme` / `Düzenle`): Markdown biçimli ve salt okunur görünür; başlıklar büyük ve kalın, vurgu, üstü çizili, iç içe listeler (imler derinliğe göre •, ◦, ▪), kod blokları (eş aralıklı, zeminli, TUI gibi dil etiketiyle renkli), alıntı (liste öğesindeki alıntı öğenin hizasında), yatay çizgi, sütun hizalı tablo, HTML varlıkları (`&copy;`). `[[not]]` bağlantısına tıklamak bekleyen düzenlemeyi önce kendi notuna kaydeder, sonra o notu açar; olmayan nota giden `[[ad]]` soluk ve üstü çizilidir, tıklanmaz; dış bağlantı yalnız ipucunda gösterilir, açılmaz. HTML düz metin kalır, görsel yerine alt metni yazılır. Seçim oturum boyunca tüm önizlenen notlarda kalır (TUI'deki `R` gibi); `csv` not tablo, `json` not girintili ve renkli görünür (TUI ile aynı kural). Büyük not (20 MiB md, json, csv) da biçimli önizlenir, yalnız görünen kısım ayrıştırılır; `limits.tui.view_bytes` ile sınır verilirse (varsayılan `0`, sınırsız) üstündeki notta önizleme TUI gibi ham metin ve soluk neden gösterir; tablo satır/sütun ve satır sınırları da TUI'nin `limits.tui.*` anahtarlarıdır.
- **Dış editör** (`Shift+E` listede, not menüsünde `Harici editörde aç`): seçili not `editor` ayarındaki programda açılır, kapanınca geri yazılır; pencere beklerken donmaz, editör açıkken kapanmaz. GUI'de bekleyen bir pencere editörü verin (`code --wait`, `gedit`); `nano` gibi terminal editörü terminalsiz başlatılan GUI'de açılamaz. **Kopyala** (`Ctrl+C` listede): notun baytları ya da klasör yolu panoya.

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (`↑↓` listede gezer, `Enter` açar) |
| `Ctrl+Shift+F` | Ad ve içerikte birlikte ara |
| `Ctrl+Shift+T` / `Ctrl+Shift+R` | Etiket seçici / kayıtlı görünüm seçici |
| `↑↓` `←→` `Enter` (listede) | Görünür satırlarda gezer; `→` klasörü açar ya da içine iner, `←` kapatır ya da üst klasöre çıkar; `Enter` notu editörde açar, Ayrıntılar'da klasöre girer |
| `Ctrl+N` / `Ctrl+S` | Yeni not / şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu |
| `Ctrl+M` | Klasöre taşı |
| `Del` (listede), araç çubuğu Sil | Notu çöpe atar, sormaz (durum çubuğunda Geri al); klasör satırında klasörü sorarak siler |
| `Shift+Del` (listede) | Kalıcı sil (sorar, varsayılan Vazgeç) |
| Araç çubuğu `Çöp (n)` | Gezgin panelinde çöp: Geri getir, Kalıcı sil, Boşalt (sorar) |
| `Ctrl+E` | PDF dışa aktar |
| `Ctrl+L` | Editör altındaki bağlantı panelini aç ya da kapat (satıra tıklamak o notu açar) |
| `Ctrl+I` | Bilgi panelini aç ya da kapat |
| `Ctrl+Shift+V` | `md`/`markdown`/`csv`/`json` notunda önizleme ↔ düzenleme |
| `Shift+E` / `Ctrl+C` (listede) | Dış editörde aç / panoya kopyala |
| `Ctrl+Shift+M` | Arama modu (Ad / İçerik) |
| Oklar / `+` `-` / `0` (harita odaktayken) | Kaydır / yakınlaştır, uzaklaştır / sığdır |
| `Shift`+sürükleme / kenara sağ tık (haritada) | İki düğümü ilişkilendir, bağla ya da etiketle / ilişkiyi ya da etiketi kaldır |
| `F5` | Şimdi senkronla |
| `Ctrl+,` (ya da araç çubuğunda ⚙) | Ayarlar; yeniden basınca Notlar |

Mevcut notlar yazmayı bıraktıktan 3 sn sonra otomatik kaydedilir (`tuning.autosave_ms`, her kayıt günlüğe gövde kadar yer tutar; kapanış ve not geçişi hemen kaydeder). Dış değişiklikler `rescan_seconds` aralığında ve
pencereye dönünce algılanır. `Ctrl+E` bir yol alanı açar (elle yaz ya da Browse); başsız/betik PDF için `note export --pdf`.
Sınırlar: ekran okuyucu desteği yok; yalnız gömülü fontlar var (DejaVu varsayılan; Ayarlar → Görünüm'den arayüz için Inter, editör için JetBrains Mono seçilir; CJK ve renkli emoji `?` kutusu çıkar, dosya bozulmaz);
PDF yalnız soldan sağa yazar (Arapça/İbranice yok). Pencere yerleşimi `gui-state.toml`'da tutulur (durum dizini).

## Yapılandırma

| | Windows | Linux |
|---|---|---|
| `config.toml` | `%APPDATA%\noteapp\` | `$XDG_CONFIG_HOME/noteapp/` (`~/.config/noteapp/`) |
| log + senkron durumu | `%LOCALAPPDATA%\noteapp\` | `$XDG_STATE_HOME/noteapp/` |
| notlar (`graph.db`) | `%LOCALAPPDATA%\noteapp\` | `$XDG_DATA_HOME/noteapp/` (`~/.local/share/noteapp/`) |
| senkron deposu (`notes_dir`) | `%USERPROFILE%\notes` | `~/notes` |

`graph.db` asıl veridir: senkron kapalıysa yedeğini sen alırsın (`note export --all DİZİN` ya da dosyanın kendisi).
`NOTEAPP_HOME` ayarlıysa config `$NOTEAPP_HOME/config`, durum `$NOTEAPP_HOME/state`, veri `$NOTEAPP_HOME/data`, varsayılan
`notes_dir` `$NOTEAPP_HOME/notes` altına gider (deneme/test için izole ortam); `config.toml`daki açık `notes_dir` değeri olduğu gibi
kalır. Ayarlar `note config`, TUI'de `,` ya da `F9`, GUI'de Ayarlar sekmesinden değişir; dosyayı elle de düzenleyebilirsin.

Dış araçlar (editör, pager, pano araçları, `hunspell`, `ssh-keygen`, dil sunucuları) yalnız PATH'in **mutlak** girdilerinde
aranır: `.`, boş girdi ya da `node_modules/.bin` gibi göreli girdiler atlanır, böylece çalışma dizinine (ör. klonlanmış
bir depoya) konmuş sahte bir `xclip`/`micro` çalışmaz. Windows'ta çocuk süreçler ayrıca
`NoDefaultCurrentDirectoryInExePath=1` ile başlar. Dizin içeren komut (`editor = "/opt/bin/ed"`) olduğu gibi denenir.

| Anahtar | Anlamı |
|---|---|
| `language` | `auto` / `tr` / `en` |
| `notes_dir` | Ayarlarda "Senkron ve geçiş klasörü": senkron deposu; notlar burada durmaz, `graph.db`'dedir (`~` ve ortam değişkenleri genişler). Değiştirmek not listesini değiştirmez |
| `editor` | Boşsa `$VISUAL` → `$EDITOR` → Windows'ta `edit`/`notepad` |
| `default_type` | Tipi belirsiz tek satırlık notun uzantısı (`md`) |
| `default_folder` | Hızlı notların klasörü (boş = kök) |
| `types` | Tip seçim listesi (elle başka uzantı da yazılabilir) |
| `icons` | Dosya ikonu: `nerd` (varsayılan) ya da `off`; eski `auto` `nerd` okunur. TUI'de terminal fontu Nerd Font olmalı; ikon boyutu terminal fontunu izler, Mono olmayan font (`MesloLGS Nerd Font`) glifin ardındaki boş hücreye taşıp daha büyük görünür; **Nerd Font yoksa glifler kutu görünür, `icons off` ayarlayın** (ikon sütunu kalkar). GUI kendi ikon fontunu taşır: `nerd` ikon çizer, `off` çizmez |
| `sync.enabled` | Senkron açık/kapalı |
| `sync.remote` | `https://…` (token) ya da `git@…` (SSH) |
| `sync.branch` | Dal (`main`) |
| `sync.idle_seconds` | TUI/GUI'de son değişiklikten kaç sn sonra senkronlanır |
| `sync.author_name` / `sync.author_email` | Commit yazarı (boşsa git config → OS kullanıcısı) |
| `lsp.enabled` / `[lsp.servers]` / `[lsp.settings.<dil>]` | TUI'de dil sunucusu (aşağıda); varsayılan açık, kurulu sunucularla çalışır; yalnız dosyadan düzenlenir |

Bozuk `config.toml` uygulamayı çökertmez (varsayılanlar + log uyarısı), `note config set` bozuk dosyada reddeder.

### Dil sunucusu (LSP, yalnız `notes`)

EDIT'te hatalar altı çizili görünür, imlecin üstündeki hatanın iletisi alt satırdadır; `Ctrl+Space` listesine sunucunun
önerileri de eklenir. Varsayılan açıktır ama yalnız **kurulu** sunucularla çalışır: program açılışta bir kez PATH'in mutlak
girdilerinde aranır (`.` ya da `node_modules/.bin` gibi göreli girdiler atlanır: klonlanmış bir depo sunucunun yerine
geçemesin); bulunamayan sunucu denenmez, hiç sunucusu bulunamayan dilin notları diske yazılmaz, o dilde ilk notu açtığında
alt satırda bir kez "pyright-langserver bulunamadı, LSP python için kapalı" yazar. Otomatik indirme yoktur; komut kabuksuz
çalışır.

Varsayılan sunucular (`resources/defaults.toml`) proje kodu çalıştırmayan ve ağa çıkmayanlardır. Üst bölüm bunu kendi
varsayılan ayarıyla sağlar; alt bölüm (★) ancak noteapp'in gömülü ayarlarıyla, aşağıdaki "Sertleştirme" tablosuna göre:

| Dil kimliği | Komut | Kurulum (Fedora) |
|---|---|---|
| `python` | `pyright-langserver --stdio` | `npm install -g pyright` |
| `c`, `cpp` | `clangd` | `sudo dnf install clang-tools-extra` |
| `shellscript` | `bash-language-server start` | `sudo dnf install nodejs-bash-language-server` |
| `markdown` | `marksman server` | [sürüm ikilisi](https://github.com/artempyanykh/marksman/releases) |
| `lua` | `lua-language-server` | [sürüm ikilisi](https://luals.github.io/#other-install) |
| `html`, `css`, `scss`, `less` | `vscode-html-language-server --stdio`, `vscode-css-language-server --stdio` | `npm i -g vscode-langservers-extracted` |
| `html` (ikinci sunucu) | `htmx-lsp` | `cargo install htmx-lsp` |
| `dockerfile` | `docker-langserver --stdio` | `npm install -g dockerfile-language-server-nodejs` |
| `sql` | `sql-language-server up --method stdio` | `npm i -g sql-language-server` |
| ★ `java` | `jdtls` | [milestone arşivi](https://download.eclipse.org/jdtls/milestones/), `bin/jdtls` PATH'e |
| ★ `go` | `gopls` | `sudo dnf install gopls` |
| ★ `rust` | `rust-analyzer` | `sudo dnf install rust-analyzer` ya da `rustup component add rust-analyzer` |
| ★ `javascript`, `typescript` | `typescript-language-server --stdio` | `npm i -g typescript typescript-language-server` |
| ★ `yaml` | `yaml-language-server --stdio` | `npm i -g yaml-language-server` |
| ★ `toml` | `taplo lsp --no-auto-config stdio` | `cargo install taplo-cli --locked --features lsp` |
| ★ `json` | `vscode-json-language-server --stdio` | `npm i -g vscode-langservers-extracted` |

Thymeleaf ve htmx şablonları `.html` olduğu için html'in iki sunucusuna da gider; tanılar birleşir ve sunucu adıyla
etiketlenir (`htmx-lsp: …`), öneriler aynı listeye eklenir, biri çökerse öteki çalışmayı sürdürür. `config.toml`'daki
girdiler varsayılanların üstüne biner; bir dili `dil = []`, hepsini `enabled = false` kapatır; bir dile birden çok sunucu
argv listelerinin listesiyle verilir:

```toml
[lsp]
enabled = true            # false: hiçbir sunucu başlamaz, dizin oluşmaz

[lsp.servers]
markdown = []             # bu dil kapalı
python = ["pylsp"]        # varsayılanın yerine
html = [["vscode-html-language-server", "--stdio"]]   # htmx-lsp'siz
```

**Sertleştirme.** Aşağıdaki ayarlar `src/edit/server_settings.cpp`'deki gömülü tablodan gelir (sunucu program adından
tanınır: `/opt/x/rust-analyzer` de `rust-analyzer`'dır) ve `initializationOptions`, `workspace/configuration` cevabı ve
`workspace/didChangeConfiguration` ile gider; her anahtar sunucunun güncel kaynağından doğrulandı:

| Sunucu | Kapatılan | Nasıl |
|---|---|---|
| rust-analyzer | `build.rs`, proc-macro, `cargo check`, bağımlılık indirme, araç zinciri indirme | `cargo.buildScripts.enable=false`, `procMacro.enable=false`, `checkOnSave.enable=false` (2023 öncesi sürümler de okur), `cargo.noDeps=true`; ortam `CARGO_NET_OFFLINE=true`, `RUSTUP_AUTO_INSTALL=0` |
| gopls | araç zinciri/modül indirme, VCS, cgo derleyicisi, komut lensleri | ortam `GOTOOLCHAIN=local`, `GOPROXY=off`, `GOVCS=*:off`, `GOFLAGS=-mod=readonly`, `CGO_ENABLED=0`; `codelenses` hepsi `false`, `vulncheck="Off"` |
| jdtls | Maven/Gradle içe aktarma, Gradle wrapper indirme, kaynak indirme, anotasyon işleme | `java.import.{maven,gradle}.enabled=false`, `java.import.gradle.wrapper.enabled=false`, `…offline.enabled=true`, `java.{maven,eclipse}.downloadSources=false`, `java.configuration.updateBuildConfiguration="disabled"`; `-data` = durum dizininde `lsp/jdtls-data` (0700) |
| typescript-language-server | otomatik tip indirme (ATA), çalışma alanındaki TypeScript/eklentiler | `disableAutomaticTypingAcquisition=true`; yerel tsserver eklentisi zaten yüklenmez (`--allowLocalPluginLoads` verilmez) |
| yaml-language-server | SchemaStore, Kubernetes CRD kataloğu, http(s) şema indirme | `yaml.schemaStore.enable=false`, `yaml.kubernetesCRDStore.enable=false`, `yaml.schemas={}`; `yaml/registerContentRequest` ile http(s) şemaları istemciye sorulur, noteapp reddeder |
| vscode-json-language-server | `$schema` URL'sinden indirme | `handledSchemaProtocols=["file"]` (http(s) istemciye sorulur, reddedilir), `json.schemas=[]` |
| taplo | şema, katalog, `taplo.toml` araması | `evenBetterToml.schema.enabled=false`, `schema.catalogs=[]`, `taplo.configFile.enabled=false`; komutta `--no-auto-config` |
| htmx-lsp | — | ayar okumaz, süreç başlatmaz, ağa çıkmaz |

Notlar sunucunun proje dosyası olduğu için şu notlar çalışma alanına **hiç yazılmaz**: `rust-analyzer.toml` (istemci
ayarını ezer, `check.overrideCommand` ile komut çalıştırabilir), `rust-toolchain.toml` (araç zinciri indirtir),
`rust-project.json`, `taplo.toml` ve `node_modules/` altındaki her şey (typescript-language-server çalışma alanının
`node_modules/typescript`'ini çalıştırır). Ad, büyük/küçük harf duyarsız dosya sistemlerinin (macOS APFS, Windows NTFS)
denk saydığı yazımlarıyla da (`ruſt-analyzer.toml`, tam genişlik harfler) reddedilir; ayrıca yazılan dosya dosya
sistemine göre engellenen adın kendisiyse silinir.

**Yalnız Windows'ta:** metninin herhangi bir yerinde ağ yolu geçen not hiçbir sunucuya gitmez ve yansıtılmaz; sunucu
böyle bir yolu açarsa Windows oraya SMB bağlantısı kurar ve parola özetin (NTLM) dışarı gider. Ağ yolu sayılanlar:
`\\sunucu\paylaşım` ya da `//sunucu/paylaşım` (önünde `:` yoksa; `https://...` sayılmaz), `file://sunucu/...` (`localhost`
dışında) ve `file:////sunucu/...`. Hangi alanda olduğuna bakılmaz (`$schema`, `#include`, `import`, tsconfig `include`,
`-I//sunucu/...`, yorum içi dahil); JSON kaçışları (`\/`, `\u002f`, `\\`) ve yüzde kodlaması (`%2F`, `%5C`) çözülmüş
hâli de taranır. Alt satırda "Bu not için LSP kapalı: ağ yolu başvurusu var (\\sunucu ya da //sunucu)" yazar. Bilinen
yanlış pozitif: protokolsüz URL (`"//cdn.example.com/x.js"`) de ağ yolu sayılır, o not Windows'ta LSP'siz kalır.
`file:///yol`, `file://localhost/yol`, `C:\a\b` ve `/a/b` yereldir. Linux ve macOS'te `//sunucu/x` yerel bir yoldur, kural
uygulanmaz. Sunucular durum dizinindeki `lsp/` klasöründe çalışır (notes'u başlattığın klasörde değil) ve ilk yansıtma
bitmeden başlamaz.

**Ayarı değiştirmek: `[lsp.settings.<dil>]`.** Bu tablo JSON'a çevrilir ve gömülü ayarın üstüne anahtar anahtar biner
(VS Code'daki ayar adlarıyla aynı noktalı yol). Yalnız yerel `config.toml`'dan okunur; notlar ve senkron verisi ayar
taşıyamaz. Tarih ve nan/inf değerleri reddedilir.

```toml
[lsp.settings.rust]
rust-analyzer.cargo.buildScripts.enable = true   # build.rs'i yeniden aç
rust-analyzer.procMacro.enable = true

[lsp.settings.yaml]
yaml.schemaStore.enable = true
```

> **Uyarı:** bir sertleştirme anahtarını geri açmak (build script, proc-macro, Maven/Gradle içe aktarma, şema indirme)
> senkronla gelen **her** notun kodunun bu makinede çalışabilmesi ya da ağdan bir şey indirtebilmesi demektir. Yalnız
> notların tamamı senin kontrolündeyse yap. Ortam değişkenleri (ör. `GOPROXY=off`) ayar tablosuyla değiştirilemez;
> gerekiyorsa sunucuyu kendi sarmalayıcı betiğinle çalıştır.

Dil notun adından ya da uzantısından gelir (büyük/küçük harf fark etmez): `c`/`h`, `cpp`/`cc`/`hpp`…, `py`, `sh`/`bash`, `md`,
`lua`, `json`, `jsonc`, `yaml`/`yml`, `toml`, `js`/`mjs`/`cjs` (`javascript`), `jsx` (`javascriptreact`), `ts`/`mts`/`cts`
(`typescript`), `tsx` (`typescriptreact`), `go`, `rs` (`rust`), `java`, `cs` (`csharp`), `html`/`htm`, `css`, `scss`, `less`,
`sql`, `Dockerfile`/`Containerfile`/`.dockerfile`, `Makefile`/`GNUmakefile`/`.mk` (`makefile`), `CMakeLists.txt`/`.cmake`,
`rb` (`ruby`), `php`, `kt`/`kts` (`kotlin`), `swift`, `zig`, `xml`, `ini`. `.env` ve `.properties` notları hiçbir sunucuya
gitmez. Bozuk girdi açılışta uyarı verir ve atlanır; çöken sunucu o oturumda kapalı kalır, nedeni alt satırda bir kez görünür.

**Gizlilik:** sunucusu tanımlı ve kurulu dillerin notları (`limits.lsp.note_bytes`, varsayılan 2 MiB'a kadar) durum dizinindeki `lsp/workspace/` altına dosya olarak
yazılır (yalnız sana açık: dizin 0700, dosyalar 0600; Linux'ta `~/.local/state/noteapp/lsp/workspace/`, Windows'ta
`%LOCALAPPDATA%\noteapp\lsp\workspace\`) ki notlar arası import çözülsün; açtığın not sunucuya gönderilir. Dizin çıkışta
silinmez, bir sonraki açılışta yeniden eşitlenir; dili listeden çıkarınca dosyaları silinir, tamamen silmek için
`enabled = false` yap ve dizini sil. Sunucu ağa çıkıyorsa içerik makineden çıkabilir. **Komut çalıştırılan bir programdır:**
`config.toml`'u dotfile deposuyla başka makinelerden eşitliyorsan oradaki `[lsp.servers]` komutları da bu makinede çalışır;
komut yalnız yerel `config.toml`'dan okunur, notlardan ya da senkron verisinden asla.

**Uyarı — senkronla gelen notlar sunucunun proje dosyası olur:** sunucusu tanımlı ve kurulu bir dilin **her** notu (yalnız açtığın
değil; senkronla başka cihazdan gelen ya da `note graph import` ile alınan notlar dahil) çalışma alanına dosya olarak
yazılır. Proje dosyalarını çalıştıran sunucular bu yüzden senkronla gelen bir notun kodunu çalıştırabilir: örneğin
rust-analyzer `build.rs`'i ve `cargo`'yu, eslint `eslint.config.js`'i çalıştırır. Notları güvenmediğin bir kaynaktan
geliyorsa yalnız proje kodu çalıştırmayan sunucuları yapılandır; bir sunucuyu eklemeden önce kendi belgesinden yapılandırma
dosyalarını, eklentileri ya da derleme betiklerini çalıştırıp çalıştırmadığına bak. Varsayılan listenin seçimi bu belgelere
dayanır, bir güvence değildir: pyright arama yollarını öğrenmek için PATH'teki `python`'u çalıştırır, clangd derleyiciyi
yalnız `--query-driver` ile izin verilirse çalıştırır (varsayılanda verilmez), sql-language-server yalnız kendi ayar
dosyanda tanımlı veritabanına bağlanır. ★ sunucular yalnız yukarıdaki gömülü ayarla güvenlidir; onları kendi
sarmalayıcınla (`["env", "rust-analyzer"]` gibi) çalıştırırsan program adı tanınmaz ve ayar gitmez. Kalan riskler:
typescript-language-server bir `tsconfig.json` notundaki `plugins`'i sunucunun kendi kurulumunun yanındaki
`node_modules`'ta arar (notlardan değil, makinede zaten kurulu paketlerden); jdtls'in veri dizini makine başına tektir,
aynı anda açılan ikinci `notes` penceresinde jdtls başlamaz. rust-analyzer'a `checkOnSave` nesne biçiminde
(`checkOnSave.enable=false`) gider: 2023 öncesi sürümler yalnız bunu okur, sonrakiler de okur.

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

**Senkronlanan tüm cihazları birlikte güncelle.** Büyük not (`limits.graph.frame_bytes`'tan, varsayılan 64 MiB, büyük) ya da
çok büyük tek işlem günlüğe yeni, parçalı biçimde yazılır. Eski sürüm bu biçimi okuyamaz: o cihazın günlüğünü bekletir ve
"uygulamayı güncelle" der; veri kaybolmaz, güncelleyince sonraki senkron devam eder. Yeni sürüm eski günlükleri okur.

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

## Kaldırma

```bash
bash scripts/install-local.sh --uninstall
```

Yalnız `~/.local/bin` bağlarını ve `~/.local/share/noteapp/<sürüm>` paket dizinlerini siler. **Notlar paketlerle aynı üst dizindedir:** `~/.local/share/noteapp/graph.db` (Windows: `%LOCALAPPDATA%\noteapp\graph.db`). **`rm -rf ~/.local/share/noteapp` notları da siler.**

Önce yedek al:

```bash
note export --all DİZİN                     # düz dosya, baytlar aynen; çöp DİZİN/.trash/ altında
note graph export --format jsonl -o yedek.jsonl  # bütün grafik, ulaşılamayan notlar dahil; note graph import ile döner
```

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
