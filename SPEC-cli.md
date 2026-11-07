# Spec: cli

> Modül: `cli` (`note` çalıştırılabiri) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Terminalden saniyeler içinde not eklemek ve notları betiklenebilir biçimde yönetmek.

## Komutlar

| Komut | Davranış |
|---|---|
| `note [-t TİP] [-f KLASÖR] [-n AD] [--no-sync] METİN…` | Argümanlar tek boşlukla birleşip not olur |
| `… \| note [bayraklar]` | stdin TTY değilse ve metin argümanı yoksa içerik stdin'den |
| `note [bayraklar]` | Metin de stdin de yoksa editör açılır (aşağıda) |
| `note -e [bayraklar] [METİN…]` | Editörü her durumda açar; METİN varsa ön içerik olur (ör. başlık yazıp devam etmek) |
| `note ls [SORGU] [-f KLASÖR] [-n N] [-l \| -0 \| --json]` | Notları listeler: sorgu varsa fuzzy sıra, yoksa mtime azalan. Sorgu SPEC-search argümanlarını alır (`tag:proje`, `view:iş`, `rel:uses`, `linked:b`, `type:md`, `sort:-size`…); geçersiz argüman değeri, bilinmeyen ya da yazan görünüm çıkış 2. Arama TUI/GUI'nin listesiyle aynı hattır (`app::Listing`): terminalde, `-l` ile ya da `--json`'da ad eşleşmelerinin altında ilişkiyle bulunan notlar nedenleriyle (`find` gibi); düz boru ve `-0` çıktısı (`-l -0` dahil) yalnız ad eşleşmelerinin yolları. Bozuk argüman iletisi yerini söyler (`Column 11: …`, değerin sütunu, TUI gibi) |
| `note find TERİM… [-n N] [-l] [--json]` | İçerik araması; `yol:satır: kesit` (renk açıkken vurgulu). Terminalde, `-l` ile ya da `--json`'da ardından içerikte bulunmayan ama etiketi, ilişki adı ya da bağlı notunun adı eşleşen notlar nedenleriyle (SPEC-search "İlişki araması"); düz boru çıktısı eskisi gibi yalnız içerik. Sorgu `ls` gibi SPEC-search argümanlarını alır (`tag:`, `type:`, `prop:`…); aranacak metin yoksa (`find tag:x`) çıkış 2; bilinmeyen anahtar (`a:b`, `http://x`) düz metindir |
| `note show SORGU` | En iyi eşleşmenin içeriğini yazdırır |
| `note edit SORGU` | En iyi eşleşmeyi editörde açar |
| `note rm YOL [-y] [-q]` | **Yalnız tam yol**; onay ister (TTY değilse `-y`/`--yes` zorunlu). Soruda okunan revizyon silmeyle aynı depo işleminde denetlenir: onay beklerken senkron notu değiştirdiyse not kalır, `Silinmedi: … onaydan sonra değişti` (çıkış 1) |
| `note mv KAYNAK HEDEF` | Taşı/yeniden adlandır; uzantı değişimi = tip değişimi; klasörler oluşur |
| `note export --pdf YOL [-o ÇIKTI] [--force]` | PDF dışa aktar (aşağıda) |
| `note sync` · `sync status` · `sync on\|off` · `sync init REMOTE [--replace]` | Senkron yönetimi (aşağıda) |
| `note sync login [HOST]` · `sync logout [HOST]` | Token'ı ekosuz stdin'den okuyup kasaya yazar / siler |
| `note config path\|list\|get ANAHTAR\|set ANAHTAR DEĞER` | Config yönetimi; `limits.*` dahil (`0` = sınırsız, SPEC-core "Sınırlar") |
| `note query [--write] [--json] [--param K=D]… CYPHER` | Graph sorgusu (aşağıda) |
| `note tags [--json]` | Canlı notların `#etiket`leri ve not sayıları, `Tag.name` sırasıyla: satır `gösterim<TAB>sayı`; `--json` `[{"name","display","count"}]` (SPEC-graph "Etiketler") |
| `note view save AD CYPHER [--force]` · `view ls [--json]` · `view run AD [--json]` · `view rm AD` · `view show AD` | Kayıtlı Cypher görünümleri (T81, SPEC-graph "Görünümler"). `save` sorguyu çalıştırmadan denetler: yazan, `$parametre`li ya da bozuk sorgu çıkış 2, hiçbir şey kaydedilmez; var olan ad (`Work` = `work`) `--force` olmadan çıkış 1. `ls` satırı `ad<TAB>sorgu` (sorgunun satır sonu `\n`), `--json` `[{"name","query","created_at","updated_at"}]` (zaman unix ms). `run` çıktısı `note query`yle (`--json` dahil) bayt bayt aynıdır, her zaman salt okunur ve görünüm profiliyle (`limits.view.*`, SPEC-graph "Görünümler") çalışır; yazan görünüm (senkronla ya da `query --write` ile gelmiş) çıkış 2 ile reddedilir, graf değişmez. `show` sorguyu yazar. Bilinmeyen ad çıkış 1. Ad ve sorgu senkronla gelebilir: terminal temizliğinden geçer. `save` ve `rm` notlar gibi arka plan senkronunu başlatır |
| `note relation add A B [--name AD] [--directed]` · `relation rm A B [--name AD \| --id KİMLİK]` · `relation rename A B ESKİ YENİ` · `relation ls [NOT] [--json]` | Adlı ilişkiler (F1, SPEC-graph "Adlı ilişkiler"). A ve B tam yoldur (`rm` gibi, fuzzy yok). `add` varsayılan adsız ve yönsüzdür; `--name` ad verir (kırpılır), `--directed` A→B yönlüdür; çiftte aynı ad anahtarı (yön ne olursa olsun) çıkış 1, geçersiz ad ya da kendine ilişki çıkış 2, olmayan not çıkış 1. `rm` çiftin tek ilişkisini, ya da `--name` (boş = adsız, `tag_key` ile) veya `--id` ile seçileni kaldırır; çiftte birden çok ilişki varken seçicisiz çağrı çıkış 1 ve ipucu; ikisi birden çıkış 2. `rename` ESKİ adlı ilişkiyi (boş = adsız) YENİ ada çevirir (boş = adsız), kimlik ve yön kalır. `ls` adları sayılarıyla verir (`gösterim<TAB>sayı`, `relation_names` sırası; `--json` `[{"name","display","count","directed"}]`; `count` adı taşıyan ilişki sayısı, `directed` bunların kaçının yönlü olduğu, bir sayı, bool değil); `ls NOT` notun ilişkilerini `yol<TAB>ad<TAB>yön<TAB>kimlik` (yön `undirected`/`outgoing`/`incoming`, kimlik 32 küçük hex; `--json` `[{"path","name","direction","id"}]`). İletiler TUI/GUI ile aynıdır (`app::map_relations`). Ad senkronla gelebilir: metin çıktısı terminal temizliğinden geçer, JSON kaçışlıdır. `add`, `rm`, `rename` arka plan senkronunu başlatır |
| `note import YOL… [--folder KLASÖR] [--type TİP] [--name AD] [--overwrite] [--json] [--no-sync]` | Dosya sisteminden not olarak içe alma; kural `app::import_paths`tedir, TUI ve GUI de onu çağırır (aşağıda "Dosya içe alma") |
| `note graph export --format jsonl [-o DOSYA]`, `note graph import [--overwrite] DOSYA` | Tüm graph'ı JSON satırları olarak çıkar ya da yükle; satırın JSON kuralları ve sınırları SPEC-graph "Sınırlar" (bilinmeyen alanda `null` kabul). İçe aktarma satır aralarında işlemlere bölünür; bir satır tek işlemdir (ya hepsi ya hiç): çerçeveden (`limits.graph.frame_ops`, `frame_bytes`) büyük satır reddedilmez; yineleme işlem yazmaz |
| `note export --all DİZİN` | Tüm notları bayt bayt düz dosya olarak çıkar |
| `note --help` · `note help KOMUT` · `note KOMUT -h\|--help` · `note --version` | Yardım (i18n, aşağıda "Yardım"); genel yardım komutları gruplu listeler |
| `note completion zsh\|bash\|fish` | Kabuk tamamlama betiğini stdout'a yazar (aşağıda "Kabuk tamamlama") |
| `note -l` · `-s` · `-S` · `-E` · `-d` · `-m` · `-x` · `-T` · `-i` · `-a` · `note cat` | Komutların kısa bayrakları ve `show` eşi (aşağıda "Kısa komut bayrakları") |

## Kısa komut bayrakları
Harfler TUI tuşlarıyla aynıdır (kullanıcı kararı 2026-11-05, `tasks/todo.md` NY-C/NY-T2). Bayrak **yalnız ilk argüman**
olarak komuttur; kalan argümanlar o komutun ayrıştırıcısına aynen gider (`note -i a.txt b.md --folder x` =
`note import a.txt b.md --folder x`, `note -s sözcük -n 5` = `note find sözcük -n 5`). Kaynak `CommandSpec::flag` ve
`CommandSpec::alias` (`command_table.hpp`); yardım ve tamamlama buradan türer.

| Kısa | Uzun | Anlam |
|---|---|---|
| `-a METİN…` | `--add` | Düz ekleme (`note METİN…` ile aynı); ilk sözcük komut adı olsa da metindir (`note -a ls`) |
| `-n AD METİN…` | `--name` | Adla ekleme (değişmedi) |
| `-l` | `--ls` | `note ls` |
| `-s` | `--find` | `note find` |
| `-S` | `--show` | `note show`; `note cat` da aynı |
| `-E` | `--edit` | `note edit` (`-e` editör seçeneği olarak kalır) |
| `-d` | `--rm` | `note rm` |
| `-m` | `--mv` | `note mv` |
| `-x` | `--export` | `note export` |
| `-T` | `--tags` | `note tags` |
| `-i` | `--import` | `note import` |

- Var olan seçenekler anlamını korur: `-t` tip, `-f` klasör, `-e` editör, `-q` sessiz, `-h` yardım. Komutun içinde
  harfler o komutundur: `note -l -l` = `note ls -l` (uzun liste).
- **Değer konumu kazanır:** değer alan seçeneğin ardındaki sözcük değerdir: `note -n -i` "-i" adlı not (metin stdin'den
  ya da editörden), `note -t -l x` tipi "-l". İlk argüman olmayan bayrak komut değildir: `note x -i` bilinmeyen
  seçenek (çıkış 2), önceki gibi.
- `--` seçenekleri bitirir: `note -- -i` "-i" metnini ekler. Komut adı olan metin `note -a ls` ya da `note -- ls`.
- Uzun eş yalnız kısa bayrağı olan komutta vardır: `--sync`, `--help` komut değildir (`--help` yardımdır).
- Eski yazımlar (`note import x`) aynen çalışır; stdout, `--json` şemaları ve çıkış kodları komut adıyla yazılmış
  hâlle bayt bayt aynıdır. `cat` artık ayrılmış sözcüktür: o metni not etmek için `note -a cat`.
- Kabuk yönlendirmeleri `note`un değil kabuğun işidir: `note -n deneme < dosya.txt` ve `cat dosya | note -n deneme`
  dosyayı "deneme" adıyla ekler, `note -S deneme | grep x` süzer, `note -S deneme >> cikti.txt` dosyanın sonuna ekler.
  `<<` dosya değil heredoc'tur: `note -n deneme <<'EOF'` sonraki satırları `EOF`'a kadar stdin olarak verir.

- Alt komut adıyla aynı metinde not eklemek için: `note -- ls`.
- "En iyi eşleşme" = tam yol varsa o, yoksa `search::rank_notes` ilk sonucu; seçilen yol stderr'e yazılır.
  Eşleşme yoksa çıkış kodu 1.
- **Terminal temizliği (T36):** stdout TTY ise `find` (yol + kesit) ve `show` not baytlarını
  `sanitize_for_terminal`'dan geçirir: C0 denetim karakterleri (`\t`, `\n` hariç), DEL, C1 (U+0080–U+009F) ve geçersiz
  UTF-8 → `U+FFFD`, `\r` atılır. Boruya/dosyaya çıktı bayt bayt aynıdır. Neden: VT işleme açıkken senkronla gelen not
  OSC 52 (pano) ya da başlık dizisi yollayabilirdi. T91: görünmez bidi ve sıfır genişlikli karakterler de (TUI/GUI ile
  aynı liste `core::hidden_controls`, ZWNJ/ZWJ hariç) `U+FFFD` olur; `ls` adları da bu yoldan geçer. `query` tablosu
  her zaman temizlenir (insan biçimi), ham değer `--json` iledir.

## Yardım
- `note help` = `note --help` = `note -h`: hızlı ekleme biçimleri, seçenekleri (`kAddOptions`) ve komutlar dört grupta
  (Notlar, Senkron, Gelişmiş, Diğer; `CommandSpec::group`), her biri tek satırlık `cli.desc_*` ile.
- `note help KOMUT` = `note KOMUT -h|--help` (bayt bayt aynı; yardım bayrağı öbür argümanları yok sayar, `--`
  sonrası metindir): başlık, `cli.help_<komut>` açıklaması, Kullanım (`cli.<komut>_usage`), Komutlar (fiiller) ve
  Seçenekler tablodan, Örnekler (`cli.examples_<komut>`, satır başına `komut<TAB>açıklama`) ve Ayrıca bakınız
  (`cli.see_also_<komut>`) katalogdan; anahtar yoksa bölüm basılmaz. Elle yazılmış seçenek listesi yoktur.
- Kısa bayraklar: genel listede komutun yanında parantezde (`note show (-S, cat)`), komut yardımında
  `cli.help_short_forms` satırı (`note -S, note --show, note cat`). `note help cat` = `note help -S` = `note help show`.
- Satır en çok 80 sütun (terminal daha darsa onun genişliği, en az 40); sözcük sınırında sarılır, sütun = kod noktası.
  Başlıklar yalnız renk açıkken kalın.

## Hata biçimi
- İlk stderr satırı bugünkü mesajdır (betikler onu arar); ardından en çok iki girintili `hint:`/`ipucu:` satırı:
  yakın sözcük önerisi (`cli::suggest`: kod noktası Levenshtein, uzaklık <= 2 ve sözcük uzunluğunun yarısından
  küçük, eşit iki aday varsa öneri yok) ve `note help KOMUT` (hızlı eklemede `note --help`). Adaylar tablodan:
  komutlar, fiiller, seçenekler. Çıkış kodları değişmez. stderr metni kararlı arayüz değildir: yalnız ilk satır ve
  çıkış kodu.
- Hızlı ekleme komut değildir: `note lss` not kaydeder (çıktı ve çıkış kodu aynı). Tek boşluksuz sözcük bir komuta
  yakınsa ve stderr TTY ise ek ipucu: `'lss' was saved as a note. Meant 'note ls'? Undo: note rm lss.md`. Otomatik
  düzeltme yok.
- `rm` onayı reddedilince çare satırı (`y` ya da `-y`/`--yes`).

## Terminal
- **Renk** (`cli::color_enabled`, stdout ve stderr ayrı): ilk eşleşen kazanır: `NOTEAPP_COLOR=always|never`;
  `NO_COLOR` boş değilse kapalı; `TERM=dumb` kapalı; akış TTY değilse kapalı; aksi açık. Renk yalnız anlam taşır
  (`find` vurgusu kalın sarı, yardım başlıkları kalın); kapalıyken bilgi kaybolmaz. `NOTEAPP_COLOR=always` boruda da
  vurgular (o zaman kesit terminal temizliğinden geçer).
- **Pager** (POSIX): tabloda `--no-pager` taşıyan komutlar (`ls`, `find`, `show`, `tags`, `config list`, `view ls`,
  `help` ve `KOMUT --help`) yalnız stdout TTY iken sayfalanır. Çıktı belleğe alınır; terminal yüksekliğine (sarılan
  satırlar dahil, `TIOCGWINSZ`, yedek `LINES`) sığıyorsa doğrudan basılır, sabit eşik yok. Komut: `NOTEAPP_PAGER` >
  `PAGER` > `less -FIRX`; `more`/`most` -> `less -FIRX`; `cat` ve `--no-pager` kapatır. Ortama (`LESS` dahil)
  dokunulmaz. Pager açıkken `note` SIGINT/SIGQUIT/SIGPIPE'ı yok sayar (Ctrl-C pager'a kalır), sonra geri yükler.
  Pager PATH'te yoksa ya da başlamazsa düz çıktı, çıkış kodu komutunki. Pager erken kapanırsa (EPIPE) sessiz biter.
  Terminal temizliği pager'dan önce yapılır. Windows'ta pager yok.
- **İlerleme:** `note sync` ve `note import` stderr TTY iken (`TERM=dumb` değil) tek satır `Syncing...` /
  `Importing n/m...` yazar (içe almada sayaç her girdide aynı satırı yeniler; payda girdilerin ön sayımı, gizliler
  hariç), iş bitince satırı siler (`\r` + `ESC[K`). Boru ve dosya hiçbir ara çıktı görmez.
- `show` TTY'de son satır sonu yoksa ekler (istem yapışmasın); boruya baytlar aynen. `tags` TTY'de sayıları tek
  sütunda hizalar; boruda TSV aynen.

## Bayraklar ve çıktı biçimleri
- Her kısa bayrağın uzun adı var (`OptionSpec.alias`): `-t/--type`, `-f/--folder`, `-n/--name`, `-e/--editor`,
  `-q/--quiet`, `-y/--yes`, `-o/--output`, `-n/--limit` (`ls`, `find`), `-l/--long`, `-0/--print0`, `-h/--help`.
  Eski kısa adlar çalışır. Değer alan uzun seçenek `--ad=DEĞER` biçimini de kabul eder; yalnız tablodaki değer alan
  uzun adda bölünür, `--` sonrası ve başka sözcükler (`note "a=b"`) aynen kalır.
- `-q/--quiet` (hızlı ekleme, `rm`, `mv`, `import`): başarı satırlarını (`Saved:`, `Deleted:`, `Moved:`, içe alma
  satırları) bastırır; hatalar, sorun satırları ve `--json` aynen.
- `note ls --json`: `[{"path","type","size","updated_at"}]` (zaman unix ms; `ls` sırası). `-l/--long`:
  `GÜNCELLEME  BOYUT  TÜR  YOL` (yerel `YYYY-MM-DD HH:MM`, boyut sağa, tür sola yaslı; başlık yok, TTY ve boru aynı).
  İlişkiyle bulunan satır `--json`'da `find --json`'un nedeni alanlarını da taşır (`reason`, `label`, `other`, `direction`/`hops`); `-l` ile satırın sonunda neden (boruda `<TAB>NEDEN`, terminalde iki boşluk).
  `-0/--print0`: kayıt ayırıcı NUL. `--limit/-n N`: en çok N satır (pozitif tam sayı, değilse çıkış 2; varsayılan
  sınırsız). `--json` `-l`/`-0`'dan önce gelir.
- `note find --json`: `[{"path","line","snippet","ranges":[[başlangıç,bitiş]…],"reason":"content"}]` (kesitte bayt
  aralıkları), ardından ilişkiyle bulunan not başına `{"path","reason","label"}` ve nedenin alanları: `"reason":"tag"`
  (`label` etiket), `"relation"` (`label` ilişki adı, `other` öbür uç, `direction` `outgoing|incoming|undirected`), `"linked"`
  (`other` adı eşleşen komşu, `hops`, `label` tek adımda kenarın adı, yoksa `""`); eşleşme yoksa `[]` ve çıkış 1.
- `note find` düz çıktısı: içerik satırları aynı. İlişki satırları yalnız terminalde (`YOL  NEDEN`), `-l/--long` ile
  (boruda `YOL<TAB>NEDEN`; neden tek satırdır, satır sonu ve sekme boşluk olur) ya da `--json`'da yazılır: düz boru çıktısı
  ve çıkış kodu eskisi gibi içeriğe bağlıdır. `--limit N` süzgeçten sonraki not sayısıdır: önce içerik eşleşmeleri, kalan yer ilişki satırlarına
  (ilişki satırları da sayılır). Terminalde, `-l` ve `--json`'da yalnız ilişkiyle bulunan sonuç da çıkış 0'dır.
- Varsayılan çıktılar (`ls`, `find`, `tags`, `query`, `view`, `config`, `show`) boruda değişmez; `find` ve `ls` terminalde ve `-l` ile
  ilişki satırı ekler, `find --json` her kayda `reason` ekler, `ls --json` ilişki satırına.

## Ortam değişkenleri
| Değişken | Etki |
|---|---|
| `NOTEAPP_HOME` | Config, veri ve log kökü (testler geçici dizin verir) |
| `NOTEAPP_COLOR` | `always` / `never` / `auto` (varsayılan): renk kararı, `NO_COLOR`'dan üstün |
| `NO_COLOR` | Boş değilse renk yok (no-color.org) |
| `TERM` | `dumb`: renk ve ilerleme satırı yok |
| `NOTEAPP_PAGER`, `PAGER` | Pager komutu (yukarıda "Terminal"); `cat` kapatır |
| `COLUMNS`, `LINES` | Pencere boyutu okunamazsa yedek |

## Hızlı ekleme kuralları

- **Tip:** `-n`'de yazılan uzantı (`-n deneme.cpp`, `a.tar.gz`, `notlar.v2`: değiştirilmez, üstüne eklenmez; SPEC-tui "Uzantı kuralı"); yoksa `-t`; yoksa `detect::suggest` (tek satır → `default_type`).
- **Ad:** `-n` (NotePath kurallarıyla doğrulanır, slug uygulanmaz) yoksa içerikten slug. **Klasör:** `-f`, yoksa `default_folder`.
- **Editör akışı:** OS geçici dizininde `noteapp-<rastgele>.<default_type>` oluşturulur → editör açılır →
  kapanınca okunur. Boş/yalnız boşluk → "iptal edildi", çıkış 1, dosya oluşmaz. Kayıt başarısız olursa geçici
  dosya **silinmez** ve yolu yazdırılır (veri kaybı yok); başarıdaysa silinir. `-e` ile metin argümanları geçici
  dosyaya ön içerik olarak yazılır; kullanıcı içeriği değiştirmeden kapatırsa ön içerik not olur.
- Çıktı tek satır: `Kaydedildi: work/plan.md (md, önerilen)` / `(java, seçilen)`.
- **Senkron:** `sync.enabled` ve `--no-sync` yoksa, yazmadan sonra ayrık arka plan süreci
  `note sync --background` başlatılır (çıktısı yok, sonucu log + sync-status'a). Komut ağ beklemez.
  `rm`, `mv` ve `edit` (dosya değiştiyse) de aynısını yapar; `--no-sync` bunlarda her konumda kabul edilir.

## Senkron komutları (T27; ayrıntı SPEC-sync karar 9)

- `note sync`: ön planda, süreç içinde tek döngü; özet satırı + her çakışma kopyasının yolu. Hata (`sync.busy`
  dahil) → çıkış 4.
- `note sync status`: açık/kapalı, `sync.remote` (ssh dışında kullanıcı bilgisi çıkarılmış), son başarı (yerel
  saat) ve son hata (`sync-status.toml`).
- `note sync on|off`: `sync.enabled`'ı yazar (bozuk config'te `config set` gibi reddeder, çıkış 1). `on`, remote
  yoksa `sync init` yönlendirmesiyle çıkış 1; varsa önce `enable(remote)` (kurulu depoda no-op).
- `note sync init REMOTE [--replace]`: `enable`; başarıda `sync.remote` + `sync.enabled = true` kaydedilir.
  Farklı origin (`sync.remote_mismatch`): TTY'de yerelleştirilmiş evet harfiyle onay, değilse çıkış 1 +
  `--replace` yönlendirmesi; `--replace` sormadan değiştirir.
- `note sync login [HOST]` / `logout [HOST]`: HOST yoksa `sync.remote`'un host'u, o da yoksa çıkış 2. Token
  TTY'de ekosuz (Windows `ENABLE_ECHO_INPUT`, POSIX `ECHO` kapalı; Ctrl-C dahil her çıkışta geri yüklenir), boruda
  ilk satırdan okunur; hiçbir yere yazdırılmaz.
- `note sync --background`: iç kullanım (yukarıdaki ayrık süreç ve TUI/GUI `AutoSync`).

## PDF dışa aktarma

`note` PDF'i süreç içinde `noteapp_pdf` ile üretir (`pdf::export_pdf`); `notes-gui` ve ekran gerekmez, GUI'siz
derlemede de çalışır. Girdi `NoteStore::read` ile okunur;
PDF yalnız bu baytlardan üretilir, dosya yeniden açılmaz. Okuma/yol hatasında hata türüne göre çıkılır (T36).
Binary (ikili) not reddedilir, çıkış 1. `pdf::export_pdf` hatasında çıkış kodu hata türünden gelir (ör. yazılamayan
çıktı: 3, yarım PDF bırakılmaz). Başarıda `cli.exported` satırı stdout'a yazılır.
Varsayılan çıktı: geçerli dizinde `<ad>.pdf`; `-o` geçerli dizine göredir. `--pdf` zorunludur, yoksa kullanım hatası (2).
Var olan çıktı (kırık bağ dahil) `--force` olmadan ezilmez: `cli.export_exists`, çıkış 1, hiçbir şey yazılmaz (TUI ve GUI
aynı durumda sorar; kural `app::export_target_exists`).
Kapsam: A4, 15 mm; Markdown (`.md`, `.markdown`) için başlık, kalın/italik, liste, kod, alıntı, bağlantı metni, GFM tablo; diğer
tipler düz metin (eş aralıklı). Yalnız soldan sağa yazı: Arapça/İbranice ve karmaşık shaping yok, Türkçe tam desteklenir.
Gömülü DejaVu'da olmayan glifler (CJK vb.) için PDF çıktısı doğrulanmadı; BMP dışı kod noktaları (emoji) `U+FFFD` olur; HTML varlığıyla yazılan BMP dışı karakter (`&#x1F600;`, NY-P) PDF'te boşluk olur (libharu yalnız BMP glifi yazar; önizlemeler onu gösterir). Görseller alt
metne (yoksa `pdf.image_placeholder`) iner; hiçbir görsel/bağlantı hedefine istek yapılmaz.

## Dosya içe alma

`note import YOL…` her yolu sırayla not yapar; biri düşerse kalanlar sürer. Kural `src/app/import_files.*`'tadır
(TUI `notes import YOL…` ve GUI "Dosya içe al" aynı kodu çağırır); özgün dosyalar okunur, hiç değiştirilmez.
- **Dosya:** baytlar aynen (BOM, CRLF, ikili; ikili not salt okunurdur, `note show` yazdırmaz). Ad dosya adından
  (`core::split_typed_name`: `a.tar.gz` -> `a.tar` + `gz`); tip uzantıdan, `--type` onun yerine geçer, uzantı yoksa içerik
  tespiti (yeni not gibi). `--name AD` yalnız tek YOL'la: dosyada not adıdır (yazılan uzantı tipi verir), klasörde hedefteki
  klasörün adı. `created_at` dosyanın doğum zamanı (yoksa ya da mtime'dan yeniyse mtime), `updated_at` mtime
  (`core::file_created_time`).
- **Klasör:** kendi adıyla `--folder` altına, alt klasörleri ve boş klasörleriyle; hedefte aynı ad varsa birleşir.
- **Ad doluysa** deponun `-2`, `-3`… adı (tavan yok); `--overwrite` bunun yerine o yoldaki notun baytlarını yazar. Asla
  sessiz ezme yok.
- **Atlananlar:** sembolik bağ/junction izlenmez (`reason.import_link`), aygıt/boru/soket (`reason.import_not_file`).
  Klasör içindeki `.` ile başlayan girdi dışarıda kalır ve sayılır (eksik sayılmaz); açıkça verilen gizli yol denenir
  (`.` ile başlayan not adı geçersizdir, `--name` ile alınır).
- **Sınır:** boyut ve sayı tavanı yok; depolamanın tek değer sınırını aşan dosya öğe hatasıdır. `tuning.max_note_bytes`
  içe almayı engellemez, yalnız notu salt okunur açtırır. Her not kendi yazma işlemindedir. Belleğe sığmayan dosya
  (`bad_alloc`, `length_error`) süreci düşürmez: `error.out_of_memory` öğe hatasıdır, kalanlar alınır.
- **Arayüzler:** TUI ve GUI aynı kuralı işçide koşar (`app::ImportJob`): ilerleme `n/m` ve şimdiki yol, durdurma girdi
  sınırında (alınanlar kalır, raporda `cancelled`, satırı `cli.import_cancelled`). Durdurulunca ne ön sayım ne
  kalanların sayımı klasörlere iner: alınmayan klasör bir girdi sayılır, durdurma ve kapanış büyük ağacı beklemez.
  Beklenmeyen istisna (depo, dosya sistemi) o girdiyi alınamayanlara yazar ve içe almayı orada durdurur (kalanlar
  `cancelled`); o ana dek alınanlar raporda kalır. CLI eşzamanlıdır, durdurma yok.
- **Hedef:** `--folder` yoksa `default_folder`. Geçersiz `--folder`/`--type` çıkış 2, hiçbir şey yazılmaz.
- **Çıktı:** her not stdout'ta `kaynak -> yol`, atlanan/alınamayan stderr'de, sonda özet. `--json`:
  `{"imported":[{"source","note","renamed","overwritten"}],"skipped":[{"source","reason"}],"failed":[{"source","error"}],"hidden":N}`.
- **Çıkış:** `0` hepsi alındı · `1` atlanan ya da alınamayan var (var olmayan yol dahil) · `2` kullanım. Yazdıysa arka plan
  senkronu başlar (`--no-sync` atlar).

## Graph sorgusu

`note query` uygulamanın deposu `<veri dizini>/graph.db` üzerinde çalışır. Dosya yoksa çıkış 1; yalnız
`--write` oluşturur. `--write` olmadan yazan sorgu çıkış 2. `--write` haritanın sorgu çubuğuyla aynı korumadan geçer
(`core::run_user_query`, SPEC-graph "Sorgu çubuğu"; F3 incelemesi, lead kararı): not gövdesi, yolu ve üst verisi,
`[[bağlantı]]`, `#etiket`, klasör ve görünüm düğümleri/kenarları ile `RELATED_TO` kurmak reddedilir (çıkış 1, konumlu
ileti; ilişki `note relation add` ile kurulur), kullanıcının kendi düğümleri, özellikleri ve var olan ilişkinin adı,
yönü, silinmesi yazılır. Okuma da aynı çekirdekten geçer (salt okunur; TUI/GUI ile aynı). Söz dizimi, desteklenmeyen öğe, eksik parametre, geçersiz
`--param`: 2. Çalışma anı hatası, korunan veri ve aşılan sınır: 1. Bozuk veya yeni sürüm dosya, G/Ç: 3. Dil: [SPEC-graph](SPEC-graph.md).
Sınırlar kullanıcının profilidir: `limits.query.*` (satır, süre, bellek, metin, iç içelik…; `0` = sınırsız, SPEC-core
"Sınırlar"). Görünüm (`note view run`, `view:`) bundan etkilenmez, `limits.view.*` ile çalışır. Graf `limits.graph.*`
ile açılır (`note graph import`/`export` de).
`--param k=v`: `null`, `true`, `false`, tam sayı, sonlu ondalık otomatik; `k="12"`, `inf`, `nan` ve aralık dışı sayı (`1e999`) metindir. Değer sorgu metnine girmez.
Tablo her zaman terminal için temizlenir (hücrede `\n` ve `\t` görünür kaçış olur, `bytes` `<bytes:N>`). Hücreler
haritanın sorgu tablosuyla aynıdır (`app::query_table`): not düğümü yolu, etiket `#ad`, başka düğüm `(:Etiket kimlik8)`,
kenar uçlarıyla (`a.md -[LINKS_TO]-> b.md`), zaman UTC `YYYY-MM-DD HH:MM:SS.mmm`; kesilmez. Yazma adlandırılamazsa tablo
yine basılır, hata stderr'e (çıkış 0: yazma commit edildi). `--json`:
`{"columns":[…],"rows":[[…]]}`; denetim karakterleri `\uXXXX`, `bytes` `{"$bytes":"<base64>"}`, zaman
`{"$timestamp":ms}`, düğüm ve kenar `{"$node":"<hex>"}`, `{"$edge":"<hex>"}`.

## Çıkış kodları
| Kod | Anlam | Örnek |
|---|---|---|
| 0 | başarı | not kaydedildi, `find` eşleşme buldu (düz boruda içerik; terminalde, `-l` ve `--json`'da ilişki de) |
| 1 | kullanıcı kaynaklı sonuç | bulunamadı (`show zzzz`), `find` eşleşme yok, iptal/onay verilmedi, `rm` TTY'siz `-y`'siz, var olan ada yazma, kısmi içe alma |
| 2 | kullanım hatası | bilinmeyen komut/fiil/seçenek, eksik değer, geçersiz yol/ad/sorgu, geçersiz `--limit` |
| 3 | G/Ç ya da iç hata | yazılamayan çıktı, bellek yetersiz |
| 4 | senkron hatası | `note sync` başarısız (yapılandırılmamış dahil) |
| 130 | SIGINT (kabuk) | Ctrl-C; uygulama özel kod üretmez |

Hatalar stderr'e, i18n mesajıyla (biçim yukarıda "Hata biçimi"). Yeni kod eklenmez.

**Config uyarıları (T36):** `load_config()` uyarıları (bozuk `config.toml` -> tüm varsayılanlar, ayıklanan remote kimlik
bilgisi, geçersiz değer) her komutta stderr'e yazılır; stdout ve çıkış kodu değişmez. Tek istisna ayrık süreç
`sync --background`: tamamen sessiz kalır.

Argüman ayrıştırma elle yazılır (ek kütüphane yok). Windows'ta UTF-8 argümanlar `wmain`/`GetCommandLineW` ile alınır.

## Kabul kriterleri (alt süreç testleri, `NOTEAPP_HOME` geçici dizinde)

1. `note "merhaba dünya"` → `merhaba-dunya.md` oluşur, içerik birebir; ≤ 200 ms.
2. `printf 'public class A {}\nimport java.util.*;' | note` → `.java` dosyası; `-t txt` ile `.txt`.
3. `-f work/2026 -n plan "x"` → `work/2026/plan.md`; `-n ../kacis` → çıkış 2, dosya yok.
4. Editör akışı `fake-editor` ile: içerik yazılırsa not oluşur; boş bırakılırsa çıkış 1, dosya yok.
5. `note rm` fuzzy sorguyla silmez; TTY yokken `-y`'siz reddeder; `-y` ile siler.
6. `note ls mtg` ve `note find "proje bütçe"` SPEC-search sıralamasını izler; Türkçe katlama çalışır.
7. `note mv a.md a.java` tipi değiştirir; hedef varsa ezmez, çıkış 1.
8. `note sync login` token'ı ekrana yansıtmaz, kasaya yazar; config/logda token yok.
9. `note config set language en` sonrası mesajlar İngilizce.
10. ~~`notes-gui` yokken `note export --pdf` anlaşılır hata ve çıkış 3 verir.~~ **EMEKLİ (2026-10-31):** `note` artık
    `notes-gui` çağırmaz. Karşılığı: `note export --pdf` eşleşmesiz sorguda çıkış 1, `--pdf`'siz kullanım hatası,
    yazılamayan çıktıda çıkış 3 ve artık dosya yok (`tests/cli/cli_export_test.cpp`).
11. `note import` CRLF+BOM'lu dosyayı `note show`da bayt bayt verir; klasör iç içe gelir, gizli girdi gelmez; dolu ad
    `-2` alır, `--overwrite` ezer; var olmayan yol çıkış 1, `--name` ile iki YOL çıkış 2 (`tests/cli/cli_import_test.cpp`,
    kural testleri `tests/app/import_files_test.cpp`: bağ atlanır, ikili salt okunur, zamanlar dosyadan).

12. Renk: pty'de `NO_COLOR=1`, `TERM=dumb`, `NOTEAPP_COLOR=never` ile `find` çıktısında `ESC` yok; boruda
    `NOTEAPP_COLOR=always` vurgular; boru varsayılanı bayt bayt aynı (`tests/cli/cli_term_test.cpp`).
13. Yardım: her komutun `-h`/`--help` çıktısı `note help KOMUT` ile bayt bayt aynı, çıkış 0, her satır <= 80 sütun,
    tablodaki her seçenek ve fiil görünür; genel yardım dört grup ve tüm komutlar (`tests/cli/cli_help_test.cpp`).
14. Hata: `note help bogus` ilk satır `Unknown command: bogus` + `hint:`; `note sync stauts` -> `status` önerisi; her
    kullanım hatasında `note help KOMUT` ipucu, çıkış 2 (`cli_help_test.cpp`, `cli_suggest_test.cpp`).
15. Yazım ipucu: pty'de `note lss` kaydeder (çıkış 0, stdout aynı) ve stderr'de ipucu; stderr dosyaya giderse ya da
    boruda ipucu yok (`cli_term_test.cpp`).
16. Bayraklar: uzun adlar ve `--ad=DEĞER` kısa adlarla aynı sonucu verir; `-q` başarı satırını bastırır, hata ve
    `--json` aynı; tamamlama betikleri yeni adları sunar (`cli_flags_test.cpp`).
17. Çıktı: boş depoda `ls --json` = `[]`; `--limit 2` iki satır; `-0` NUL; `-l` hizalı; `find --json` aralıklarla;
    varsayılan `ls` aynı (`cli_output_test.cpp`).
18. Pager: pty'de uzun `ls` `PAGER`'dan geçer, sığan çıktı geçmez; `NOTEAPP_PAGER=cat`, `--no-pager`, olmayan pager
    düz çıktı ve çıkış 0; boruda pager yok (`cli_term_test.cpp`).
19. Küçükler ve ilerleme: pty'de `show` satır sonuyla biter; `rm` `yes` yanıtı reddedilir + ipucu; `tags` hizalı;
    `sync`/`import` ilerleme satırı yazıp siler, stderr dosyada ve `TERM=dumb`'da ara çıktı yok (`cli_term_test.cpp`).
20. Kısa komut bayrakları: her kısa ve uzun bayrak ilk argümanda komutu seçer, kalan argümanlar aynen gider; `-l`,
    `-T`, `-s` çıktısı ve `--json`'u uzun yazımla aynı; `cat` = `show`; `-a ls` metin ekler; `-n -i` adı "-i";
    ilk olmayan bayrak çıkış 2; yardım ve üç kabuğun tamamlaması bayrakları sunar (`cli_short_flags_test.cpp`,
    `completion_shell_test.sh`).
21. İlişkiler: `relation add` adsız/adlı/yönlü kurar, çift ad çıkış 1, geçersiz ad ve kendine ilişki çıkış 2; `ls` adları sayılarıyla ve `ls NOT` satırları kimlikle (`--json` dahil); `rm` tek ilişkiyi, `--name` ya da `--id` ile seçileni kaldırır, belirsizde çıkış 1; `rename` adı değiştirir; senkronla gelen bozuk ad denetim ve görünmez karakter olmadan yazılır (`tests/cli/cli_relation_test.cpp`).

**Elle:** Windows Terminal ve Fedora'da `edit`/`micro`/`nano`/`vim` ile editör akışı; Türkçe karakterli argümanlar;
Windows Terminal'de renk ve ilerleme satırı (VT kipi), pager yokluğu.

## Kabuk tamamlama
- Tek kaynak `src/cli/command_table.hpp`: komutlar, fiiller, seçenekler, değer türleri ve i18n açıklama anahtarları.
  Ayrılmış sözcükler ve üç betik buradan türer; yeni komut/seçenek önce tabloya girer.
- Dinamik değerler gizli `note __complete TÜR [ÖNEK]` ile gelir (yardımda ve tamamlamada görünmez, ayrılmış sözcük):
  `note`, `folder`, `type` (config `types`), `tag`, `view`, `config-key` (`limits.*` dahil, `sync.token` yok), `query`
  (iki noktasız son sözcükte anahtar adları `search::query_key_names`, `tag:` `type:` `in:` `view:` sonrasında değerler;
  önceki sözcükler aynen kalır). Satır başına bir aday; denetim karakteri, satır sonu, bozuk UTF-8 taşıyan aday yazılmaz.
  Salt okur: depo yoksa kurulmaz, türetilmiş dizin onarımı atlanır; açılamayan
  depo boş çıktı ve çıkış 0. stderr'e hiçbir şey yazmaz. Bilinmeyen tür çıkış 2.
- Dosya yolları (`import`, `-o`, `graph import`) kabuğun kendi tamamlamasıdır.
- Kısa komut bayrakları ve `cat` ilk sözcükte önerilir (`note -<Tab>`: bayraklar ve hızlı ekleme seçenekleri); betik
  ilk sözcüğü komut adına çevirir, `note -S <Tab>` `note show <Tab>` gibi tamamlar. Sonraki sözcükte önerilmez.

## Kapsam dışı
- `note font install`: gerekmiyor; GUI kendi ikon fontunu taşır, terminalde font kullanıcınındır (SPEC-tui "Dosya ikonları").
- PowerShell tamamlaması (bash/zsh/fish var).

Etkileşimli seçici (fzf benzeri seçim TUI'nin işi), `mkdir/rmdir` komutları
(klasör `-f`/`mv` ile örtük oluşur; klasör silme TUI/GUI'de).

## Değişiklik 2026-10-31
- **MODIFIED** PDF dışa aktarma: `note`, `noteapp_pdf`'e doğrudan link olur ve PDF'i süreç içinde üretir. Neden: GUI'siz
  derlemede ve ekransız sunucuda çalışsın (`tasks/plan-imgui.md` davranış 5).
- **REMOVED** `notes-gui --export-pdf` çağrısı, `QT_QPA_PLATFORM` ortamı, `NOTEAPP_GUI_EXE` test dikişi ve `cli.gui_not_installed` hatası.
- **REMOVED** Kabul 10 (`notes-gui` yokken çıkış 3): emekli, numara korundu; karşılığı kabul 10 satırında.
- **MODIFIED** PDF içeriği: yalnız soldan sağa basit metin; Markdown kapsamı yukarıda (davranış 6). Çıktı artık Qt yerine libharu + md4c ile üretilir.


## Değişiklik 2026-11-04
- **ADDED** Kabuk tamamlama (`note completion zsh|bash|fish`, gizli `note __complete`); önceki "Kapsam dışı" maddesinden
  çıktı. Neden: kullanıcı isteği (zsh + fzf-tab). `completion` ve `__complete` artık ayrılmış sözcük: o metni not etmek
  için `note -- completion`.

## Değişiklik 2026-11-05 (CLI yeniden tasarım, T1-T12)
- **ADDED** "Yardım", "Hata biçimi", "Terminal", "Bayraklar ve çıktı biçimleri", "Ortam değişkenleri" bölümleri ve
  kabul 12-19. Neden: yazım hatası sessizce not oluyordu, alt komutlarda `--help` yoktu, `NO_COLOR` yok sayılıyordu,
  makine çıktısı tutarsızdı, uzun çıktı sayfalanmıyordu.
- **ADDED** `-h/--help` her komutta; `ls --json|-l|-0|--limit`, `find --json|-n`; uzun bayrak adları, `--ad=DEĞER`,
  `--yes`, `-q/--quiet`, `--no-pager`; `NOTEAPP_COLOR`, `NOTEAPP_PAGER`.
- **MODIFIED** Yardım metni tablodan üretilir; `cli.help_<komut>` artık yalnız açıklama paragrafıdır, `cli.help`
  hızlı ekleme biçimlerinin listesidir. Kullanım hatalarına `hint:` satırları eklendi; ilk satır, stdout ve çıkış
  kodları aynı.
- **MODIFIED** `find` vurgusu artık "stdout TTY" yerine `color_enabled` ile karar verir.
- **Kapsam dışı (bilinçli):** `--color` bayrağı (env yeter), kısa/uzun yardım ayrımı, alt komut kısaltması,
  `-v/--verbose`, hata başlığının kırmızı renklendirilmesi, `config list --json` ve `sync status --json`, Windows
  pager. (`import` `n/m` sayacı NX-C'de geldi: "İlerleme".)

## Değişiklik 2026-11-05 (NY-C kısa komut bayrakları)
- **ADDED** "Kısa komut bayrakları": ilk argümanda `-l -s -S -E -d -m -x -T -i` ve uzun eşleri komutu seçer, `-a/--add`
  düz ekleme, `note cat` = `show`; kabul 20. Neden: kullanıcı kararı, harfler TUI tuşlarıyla aynı (NY-T2).
- **MODIFIED** Ayrılmış sözcükler: `cat` ve komut bayrakları. Daha önce `note cat` "cat" notunu ekliyordu, `note -l`
  bilinmeyen seçenekti (çıkış 2); şimdi ikisi de komuttur. Eski yazımların stdout'u, `--json`'u ve çıkış kodları aynı.

## Değişiklik 2026-11-06 (F1 adlı ilişki, CLI)
- **ADDED** `note relation add|rm|rename|ls` ve kabul 21. Neden: kullanıcı isteği "ilişki kurulurken edge'e isim verilebilsin" (plan-graph-map F1); GUI/TUI ile aynı kural ve iletiler (`app::map_relations`).
- **MODIFIED** Ayrılmış sözcükler: `relation`. Daha önce `note relation` bilinmeyen komuttu (çıkış 2).

## Değişiklik 2026-11-06 (ilişki araması, CLI)
- **MODIFIED** `note find` (`-s`): içerik satırlarından sonra etiket, ilişki adı ya da bağlı notun adıyla bulunan notlar yalnız yollarıyla gelir;
  terminalde, `-l` ve `--json`'da yalnız ilişkiyle bulunan sonuç da çıkış 0'dır; düz boru çıktısı ve çıkış kodu eskisi gibidir
  (inceleme kararı: betikler bozulmaz). Neden: kullanıcı isteği "arama yalnız içerik ya da not araması olmasın, graftaki ilişkiler de
  dikkate alınsın" (TUI/GUI ile aynı kural, `search::relation_hits`). Eski içerik satırları ve sıraları aynı.
- **ADDED** `note find -l/--long` (nedeni yazar) ve `--json`'da `reason` alanı (içerik kaydında `"content"`).
- **ADDED** `note ls` sorgusunda `rel:` ve `linked:` (SPEC-search).

## Değişiklik 2026-11-06 (F4 özellikler, CLI)
- **ADDED** `note prop get NOT [ANAHTAR] [--json]`, `note prop set NOT ANAHTAR DEĞER [--type text|number|bool]`, `note prop rm NOT ANAHTAR`; `note relation prop get|set|rm KİMLİK …` (kimlik `note relation ls NOT`tan). Kural SPEC-graph "Özellikler (F4)", ileti `app::Inspector` (GUI/TUI ile aynı).
  - `get`: `ANAHTAR<TAB>TÜR<TAB>DEĞER`, uygulamanın anahtarında sonda `<TAB>read-only`; `--json` `[{"key","type","value","read_only"}]` (sayı ve bool yalın, büyük değer null). Tek anahtar yalnız değeri yazar (`--json`'da JSON değeri). Çıktı `sanitize_for_terminal`'dan geçer.
  - `set`: tür değerden (önek ya da kendiliğinden) ya da `--type`; eksi sayı (`-5`) değerdir, `-` ile başlayan metin `--` sonrası verilir. `rm`: olmayan anahtar 1.
  - Çıkış: ayrılmış ya da geçersiz anahtar, geçersiz değer, bilinmeyen tür, bozuk kimlik, kullanım 2; olmayan not, ilişki ya da anahtar 1.
- **ADDED** `note ls`/`find` sorgusunda `prop:anahtar` ve `prop:anahtar=değer` (SPEC-search).
- **MODIFIED** Ayrılmış sözcükler: `prop`. Daha önce `note prop` metin olarak eklenirdi.

## Değişiklik 2026-11-07 (SD9: tek çekirdek, CLI)
- **MODIFIED** `note ls`: ad aramasında ilişkiyle bulunan notlar terminalde, `-l` ve `--json`'da nedenleriyle gelir; düz boru ve
  `-0` çıktısı aynı. Neden: kullanıcı ilkesi "tek uygulama, üç arayüz" ve NY-S (aramada ilişkiler): CLI `ls` TUI/GUI'nin
  `app::Listing` hattını kendisi yeniden yazıyordu ve ilişkiyi atlıyordu.
- **MODIFIED** `note find`: aynı hat (`app::Listing`, içerik kipi); sorgu argümanları (`tag:`, `type:`, `prop:`…) süzer, bozuk değer
  ve metinsiz sorgu çıkış 2. Daha önce argümanlar düz metin aranıyordu (yukarıdaki `find` `prop:` maddesi tutmuyordu).
- **MODIFIED** `note query` okuma ve `note view run`: `core::run_user_query` / `core::run_saved_view` (TUI/GUI ile aynı); tablo
  hücreleri `app::query_table`: not düğümü onaltılık kimlik yerine yolu, kenar uçlarıyla, zaman UTC metin (önce unix ms).
  `--json`, çıkış kodları ve "depo yok" iletisi aynı.
- **FIXED** (SD9 inceleme) `find -n N` süzgeçten sonra keser: önce kesildiğinde süzülen isabet yerini harcıyor, `find kelime type:txt -n 1`
  .txt eşleşmesi varken boş dönüyordu. `ls -l -0` NUL akışına ilişki satırı yazmaz. Bozuk argüman iletisi sütunu söyler.
