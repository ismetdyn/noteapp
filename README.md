# noteapp

Terminalde yaşayan biri için hızlı, sunucusuz, sahibine ait bir not uygulaması. Tek uygulama, üç arayüz, ortak bir çekirdek:

| Komut | Arayüz |
|---|---|
| `note` | CLI (betik ve hızlı ekleme için) |
| `notes` | TUI: iki panelli, FTXUI |
| `notes-gui` | Masaüstü GUI: SDL3 + Dear ImGui |

Notlar veri dizinindeki tek bir gömülü SQLite graph veritabanında (`graph.db`) durur; baytlar aynen saklanır,
`note export --all DİZİN` hepsini düz dosya olarak geri verir. Veritabanı bağlantıları (`[[not]]`), `#etiket`leri, adlı
ilişkileri ve özellikleri (properties) tutar; Cypher benzeri sorgularla okunur. Notlar istenirse private bir GitHub
reposu üzerinden cihazlar arasında (Windows ↔ Linux) senkron kalır. Üç arayüz aynı notları ve aynı `config.toml`'u paylaşır.

## Kurulum

### Hazır paket (GitHub Releases)

- **Linux:** `noteapp-0.1.0-x86_64.AppImage`. Çalıştırmak için glibc ≥ 2.38 ve FUSE gerekir (Fedora: `sudo dnf install fuse3`).
  ```bash
  chmod +x noteapp-0.1.0-x86_64.AppImage
  ./noteapp-0.1.0-x86_64.AppImage              # argümansız: notes-gui
  ./noteapp-0.1.0-x86_64.AppImage note ls      # ilk argüman: note | notes | gui
  ```
  Dosya adı komutu da belirler: `note`, `notes`, `notes-gui` adlı symlink'ler üçünü ayrı komut yapar
  (`packaging/linux/AppRun`). Günlük kullanım için yerel kurulum daha hızlıdır: `bash scripts/install-local.sh <AppImage>`.
- **Windows 11:** `noteapp-0.1.0-windows-x86_64.zip` içinde `note.exe`, `notes.exe`, `notes-gui.exe` ve `licenses/`.
  Açıp çalıştır; üç exe'yi aynı klasörde tut (`notes` ve `notes-gui` `note`'u kendi yanında arar). İmzasızdır,
  SmartScreen uyarısı verebilir.

### Kaynaktan derleme

Gerekenler: C++23 derleyici, CMake ≥ 3.28, Ninja, `patch`; bağımlılıklar derlemede indirilir (ağ gerekir).

```bash
cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release   # Linux (Fedora paket listesi: docker/fedora.Containerfile)
bash scripts/build-windows.sh        # Windows .exe'leri, Linux'ta podman/docker ile çapraz derleme
bash scripts/build-appimage.sh       # Linux AppImage (docker/podman)
```

Testler `pdftotext` ister (`poppler-utils`); testsiz derleme için `-DNOTEAPP_BUILD_TESTS=OFF`, GUI'siz için
`-DNOTEAPP_BUILD_GUI=OFF`. Ayrıntı ve macOS/Windows yerel derleme: [SPEC.md](SPEC.md).

> Windows paketleri gerçek Windows'ta henüz sınanmadı (yalnız Wine'da).

## Hızlı başlangıç

### CLI: `note`

```bash
note "toplantı notu: bütçe onayı #iş"        # hızlı not (tip içerikten önerilir)
note -f work/2026 -n plan "ilk taslak"       # klasör + ad → work/2026/plan.md
note ls "plan tag:iş type:md"                # liste/arama: tag:, type:, in:, rel:, linked:, view:
note ls prop:durum=bitti                     # özelliğe göre süz
note show plan                               # en iyi eşleşmeyi yazdır (note edit: editörde aç)
note find bütçe                              # içerik araması → yol:satır: kesit
note relation add a.md b.md --name kullanır --directed
note query "MATCH (n:Note)-[:TAGGED]->(t:Tag {name:'iş'}) RETURN n.name"
note export --all yedek/                     # tüm notlar düz dosya; --pdf YOL tek not PDF
note sync                                    # elle senkron (kurulum: aşağıda)
```

Tüm komutlar: `note --help`, tek komut: `note help KOMUT`. Kabuk tamamlama: `note completion zsh|bash|fish`.
Çıkış kodları ve kısa bayraklar: [SPEC-cli.md](SPEC-cli.md).

### TUI: `notes`

`notes` komutu iki panel açar (liste + önizleme). Eşleşme yoksa aramada `Enter` yeni not açar.

| Tuş | Eylem |
|---|---|
| `F1` / `?` | Tüm tuşlar (yardım) |
| `/` | Ara (`tag:`, `type:`, `view:`... argümanlarıyla) |
| `a` / `n` | Yeni not / yeni not ya da klasör |
| `Enter` / `e` | Düzenle (`Ctrl+S` kaydet, `Ctrl+Q` çık) |
| `:` | Komut paleti: her komut adıyla aranır (harita: `view.map`) |
| `Q` (haritada) | Cypher sorgu satırı |
| `,` / `F9` | Ayarlar |
| `q` | Çık |

Tuşların tamamı: [SPEC-tui.md](SPEC-tui.md).

### GUI: `notes-gui`

Üstte tek araç çubuğu (yeni not/klasör, içe al, yeniden adlandır, taşı, sil, çöp, PDF) ve sağda görünüm seçici:
**Ağaç**, **Ayrıntılar**, **Harita** (notlar çizge olarak). Solda liste, sağda editör; `Ctrl+K` aramaya odaklanır,
`Ctrl+N` yeni not, `F5` senkron, `Ctrl+,` ayarlar. Notlar yazmayı bıraktıktan kısa süre sonra kendiliğinden
kaydedilir. Ayrıntı: [SPEC-gui.md](SPEC-gui.md).

## Nasıl çalışır

- **Depolama:** asıl veri `graph.db`'dedir (senkron kapalıyken yedeği sen alırsın: `note export --all`). Dosyalar
  değil, graph kaynaktır; dışa aktarma düz dosya üretir.
- **Bağlantılar ve etiketler:** `[[hedef]]` bağlantı, `#etiket` (iç içe `#proje/alt`) etiket olur; ikisi de metinden türer.
- **Adlı ilişkiler ve özellikler:** metinden bağımsız, notlar arasında adlı (isteğe bağlı yönlü) ilişkiler kurulur;
  notlara ve ilişkilere anahtar-değer özellikleri yazılır (`note relation`, `note prop`).
- **Sorgu ve görünümler:** `note query` Cypher alt kümesiyle graph'ı okur (`--write` olmadan salt okunur). Sorgu
  `note view save AD "..."` ile kayıtlı görünüm olur ve aramalarda `view:AD` ile süzer. Harita bağlantı ve ilişkileri çizer.
- **Senkron:** git yalnız taşıyıcıdır; depoya her cihazın işlem günlüğü (`graph/<cihaz>.oplog`) commit'lenir ve
  cihazlar birleşir. Aynı not iki cihazda değişirse bir sürüm yerinde kalır, diğeri `ad (conflict ...)` olarak yanına
  yazılır; farklı satırlar otomatik birleşir, hiçbir şey kaybolmaz.
  ```bash
  note sync login github.com                                   # token'ı OS kasasına yazar
  note sync init https://github.com/<kullanici>/notes.git      # boş, private repo
  note sync status
  ```
  Repo özel olmalı, şifreleme yoktur. SSH ve sorun giderme: [SPEC-sync.md](SPEC-sync.md).
- **Yapılandırma:** tek `config.toml` (`note config path | list | get | set`; TUI'de `,`, GUI'de Ayarlar).
  Linux: `~/.config/noteapp/`, Windows: `%APPDATA%\noteapp\`. Veri `~/.local/share/noteapp/` (Windows:
  `%LOCALAPPDATA%\noteapp\`). `NOTEAPP_HOME` ayarlıysa config/state/data onun altına gider (izole deneme ortamı).
  Sınırlar `limits.*` anahtarlarıyla ayarlanır, `0` sınırsız demektir.

## Belgeler

[SPEC.md](SPEC.md) (harita, teknoloji, sözleşmeler) · [core](SPEC-core.md) · [graph](SPEC-graph.md) ·
[search](SPEC-search.md) · [detect](SPEC-detect.md) · [sync](SPEC-sync.md) · [cli](SPEC-cli.md) ·
[tui](SPEC-tui.md) · [gui](SPEC-gui.md)

Geliştirme: `ctest --preset linux-gcc-debug --output-on-failure`, `scripts/check-fedora.sh` (Fedora imajında derle + test).

## Lisans

[GPL-3.0-or-later](LICENSE). Kullanılan bileşenlerin ve gömülü fontların
(DejaVu, Inter, JetBrains Mono) lisansları Windows paketinde `licenses/`, AppImage içinde `usr/share/licenses/noteapp/`
altındadır.
