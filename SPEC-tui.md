# Spec: tui

> Modül: `tui` (`notes` çalıştırılabiri, FTXUI) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

`notes` yazınca açılan, klavyeyle fzf hızında gezinilen iki panelli not uygulaması. Önizleme `cat` gibi okunur, düzenleme `micro` gibi hissettirir, ikisi de sağ panelde. Düzenleme sağ panelde, yeni not (`n`) tüm terminali alan gömülü editörde olur; harici editör yalnız açıkça `E` ile.

## Yerleşim

```
notlar/work                                       [ad] 42/5000   ✓ 14:32
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓┌──────────────────────────────────────┐
┃ 1 ▾ work/                  ┃│# Toplantı notları                    │
┃   1   plan.md              ┃│- bütçe onayı                         │
┃ 2 ▸ personal/              ┃│...                                   │
┃ 3   meeting-notes.md       ┃│                                      │
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛└──────────────────────────────────────┘
Enter düzenle  → önizle  / ara  n yeni  a ekle  d sil  ? yardım  ...
```
- Sol: geçerli klasörün çocukları (bkz. "Klasör görünümü"); aramada ve çöpte tam yollu düz liste, eşleşen karakterler vurgulu. Sağ: Liste ve Aramada seçili notun önizlemesi; seçim değişince aynı karede güncellenir. Önizlemede uzun satır kırpılmaz, panele sarılır (`cat`). Editör odağında aynı panel gömülü editördür; editörden seçim değişmez.
- Üst: klasör yolu, mod (`[ad]` / `[içerik]`), süzgeçte eşleşme/toplam, klasör görünümünde ve çöpte yalnız not sayısı (`6 not`; satır klasör de sayar, oran yanıltır), senkron durumu (`kapalı` / `✓ saat` / `⟳` / `⚠`).
- Alt: odağa göre kısayol çubuğu (i18n; Editörde üstünde durum çubuğu) ya da tek satırlık girdi (arama, not içi arama, ad istemi, yol girdisi). Katalogdaki sıra önem sırasıdır; sığmayan ipucu ve ardındakiler bütün olarak düşer. Listede `T`, `Space`, `o`, `v`, `f`, `?` her genişlikte (60 sütundan) görünür, seyrek olanlar `?`tedir. Listede kalan süzgeç (`> sorgu`) ve Önizlemede eşleşme sayacı (`3/12`) ipuçlarının başındadır; uzun süzgecin sonu kısalır, ilk beş ipucunu düşürmez. En küçük boyut 60×15; altında "pencere çok küçük" mesajı.
- **Odak görünümü:** odaklı panel kalın, kipe göre renkli ve etiketli çerçeve alır, diğeri ince: `LIST` camgöbeği, `REVIEW` yeşil (Önizleme ve salt-okunur not), `EDIT` eflatun, kaydedilmemişse `EDIT *`. Renk tek sinyal değildir. Girdide imleç `▏`, Editörde gerçek terminal imleci. Seçim yalnız satır vurgusudur: liste odağında ters renk, sağ panel odaklıyken soluk arka plan. Satır: seçim işareti hücresi (`*`), girinti, sağa yaslı soluk numara (genişlik en büyük numaradan), klasörde `▸` (kapalı) ya da `▾` (açık), not satırında aynı iki hücre boş, ikon, ad, sağ sütun. Notun açık ayrıntısı yalnız girintili bloktur, işareti yoktur. Oka tıklamak klasörü yerinde açar/kapatır; not satırına tıklamak yalnız seçer.
- **Toplamlar:** kısayol çubuğunun üstünde klasör görünümünde `Klasör: 12 öğe · Toplam 248 not · sayfa 2/7`, süzgeçte `Toplam 248 not · 12 eşleşme · sayfa 2/7` (eşleşme yalnız süzgeçte; sayfa = seçili satır / panel yüksekliği, kaydırma sanaldır). Editörde aynı satırı durum çubuğu kullanır.
- **Sağ sütun (`v`):** varsayılan boyut + tarih (`2.0 KiB  12:03`, bugün değilse yalnız tarih). Dar panelde önce tarih, sonra boyut düşer, değer yarım kesilmez; yol en az 16 hücre tutar. `created` ve `origin` boşken "bilinmiyor".
- **Üst satır:** çöp doluysa en solda `çöp: 3`, tıklayınca çöp görünümü açılır; boşsa yok. Yalnız metin: simge karakterinin genişliği terminale göre değişir, satır kayardı. Çöp görünümünde aynı yerde `ÇÖP KUTUSU` başlığı.
- **Boyut:** Liste ve Önizlemede `<` `>` sol paneli 5 sütun değiştirir (sol en az 16, sağ en az 24 sütun; 60 sütunda da geçerli), `z` odaklı paneli tam ekran yapar ve geri alır (F11 değil: Konsole'un). Fareyle bölücü sürüklenir. Genişlik ve yakınlaştırma oturumluktur, config'e yazılmaz. Yakınlaştırmada odak değişirse gösterilen panel de değişir.
- **Fare:** bölücü sürükleme, tekerlek işaretçinin altındaki paneli kaydırır (liste seçimi / önizleme / editörde imleç), satıra tıklama seçer, panele tıklama odaklar; Editörde tıklama imleci o hücreye koyar (numara sütunu metin değildir, üstüne tıklama satır başıdır). Listeye tıklamak editörden `Ctrl+Q` ile aynı kapıdan çıkar (kirliyse sorar). Açık soru, girdi ya da modal fareyi yok sayar. REVIEW'da sürükleme metin seçer (bkz. "Pano"); fare yakalandığı için terminalin kendi seçimi `Shift`+sürükle ile yapılır (yardımda yazar).

## Tuşlar

Dört odak vardır: **Liste** (açılış), **Arama**, **Önizleme**, **Editör**. Harfler aynı anda hem süzüp hem eylem yapamaz; harf kısayolları Liste ve Önizlemede, yazı girdilerde ve Editörde çalışır. Diyalog, ayarlar, yardım ya da tek satırlık girdi açıkken tüm tuşlar onlara gider. Harf eşleşmesi birebirdir: `ı` ve bilinmeyen harfler yok sayılır.

**Liste**

| Tuş | Eylem |
|---|---|
| `↑` `k`, `↓` `j` | Listede gezin, sağ panel canlı izler |
| `gg` `G` | İlk / son satır. `g` öneki sıradaki tuşu bekler, alt satır ne beklediğini söyler (süre yok): `gg` baş, `gh` kök klasör, `gt` çöp; başka tuş vazgeçer ve başka iş yapmaz |
| `x` `X` `Ctrl+A` | Satırı seç ve aşağı in, seçimi temizle, görünen satırları seç (bkz. "Çoklu seçim ve yank") |
| `y` `p` | Seçimi ya da satırı kopya için işaretle, geçerli klasöre yapıştır |
| `PgUp` `PgDn` | Önizlemeyi kaydır |
| `→` `l` | Notta önizleme odağı, klasörde içeri girer |
| `←` `h` | Üst klasör, seçim çıkılan klasörde kalır; kökte etkisiz. Arama sonucunda notun klasörüne gider |
| `Enter` `e` | Seçili notu sağ panelde düzenle, klasörde içeri girer; liste boşsa etkisiz |
| `E` | Harici editörde aç (`EDITOR`); terminali ona bırakır |
| `F2` | Altta tek satırlık yol girdisi: klasör görünümünde geçerli klasöre göre ad ön dolu (düz listede tam yol), `/` içeren yol taşır, imleç adın sonunda; klasörde `rename_folder`; `Enter` `NoteStore::move` (hedef doluysa hata girdide kalır, ezme yok), `Esc` vazgeçer. Önizlemede de çalışır |
| `PgUp` `PgDn` | Listede sayfa sayfa gezer (önizleme yalnız kendi odağında kayar); `gg` `G` baş ve son |
| `,` | Ayarlar (Önizlemede de); `F9` her odakta |
| `/` | Arama girdisi (altta) |
| `n` `Ctrl-O` | Yeni not: tüm terminali alan boş editör |
| `a` | Listenin başında odaklı ad satırı, ad geçerli klasöre göredir; `/` ile biten ad klasör oluşturur, Listede kalır, klasör seçilir, editör açılmaz (bkz. "Yeni not", "Klasör görünümü"); `Enter` boş notu oluşturur ve sağ panelde EDIT'te açar; `Esc` ve boş girdi hiçbir şey yazmaz; geçersiz ya da dolu ad satırın altında kırmızı görünür, satır metniyle açık kalır |
| `f` | Genel arama girdisi (altta): ad ve içerik birlikte (`search_all`), içerik eşleşmesinin en iyi satırı yolun altında; `Enter` Önizlemede eşleşmeye kayar; Listede `Esc` kapatır |
| `d` `Ctrl-D` | Çöpe at, onaysız; seçim komşu satıra geçer. `u` bu oturumda son atılanı geri alır. Klasörde bkz. "Klasör görünümü" |
| `D` | Kalıcı sil (onay, varsayılan "Vazgeç") |
| `T` | Çöp görünümü: `↑↓` gezin, `u` seçileni geri getir, `D` kalıcı sil (onay), `E` tümünü boşalt (onay, varsayılan "Vazgeç"), `q` `Esc` `T` geri. Liste komutları çalışmaz. Sağ panel çöpteki notu salt okunur gösterir (`PgUp` `PgDn` ve tekerlek kaydırır; odak listede kalır) |
| `c` | Klasörde yolunu kopyalar; notta özgün baytlarını sistem panosuna kopyalar; ikili ya da UTF-8 olmayan notta nedenini söyler, kopyalamaz |
| `Space` | Klasörü yerinde aç/kapat (bkz. "Ağaç"); notta ayrıntıyı aç/kapat |
| `i` `I` | Seçili satırın ayrıntısı, klasörde de; `I` tüm satırları. `Alt+→` `Alt+←` aynı işi yapar ama her terminalde gelmez |
| `m` | Seçimi ya da satırı klasöre taşı: altta tek satırlık girdi, geçerli klasörle dolu (bkz. "Ağaç") |
| `Alt+→` `Alt+←` | Satırı aç / kapat: boyut, güncelleme, oluşturma, senkron, kaynak; `created` ve `origin` boşken "bilinmiyor". Birden çok satır açık kalır, gezinme not satırlarını yürür |
| `v` | Sağ sütun döngüsü: boyut + tarih, boyut, güncelleme, oluşturma, senkron, kaynak, yok. `tui-state.toml`da `version` yoksa eski `column = "none"` yeni varsayılana taşınır |
| `o` | Sıralama menüsü (alt satır): `n` ad, `u` güncelleme, `c` oluşturma, `s` boyut, `r` ters, `Esc` vazgeç; etkin sıra üst satırda |
| `r` | Ad/klasör/tip diyaloğu; klasörde `F2` |
| `s` | Şimdi senkronla |
| `<` `>` `z` | Panel genişliği, yakınlaştırma |
| `?` | Yardım |
| `q` | Çık |
| `Tab` | Ad ↔ içerik modu (Aramada `Ctrl-T`) |
| `Esc` | Bekleyen yank varsa ondan vazgeç; süzgeç doluysa temizle; boşsa etkisiz, çıkmaz |

Sıra ve sağ sütun `<state>/tui-state.toml`'da kalır; bozuk değer varsayılana döner (`updated` azalan, sütun yok).

`/` ve `f` her zaman kökten, klasörler arası düz sonuç verir: tam yol, sonuçlar boyunca numara. `/` metin yazılana dek klasör görünümünü bırakır. `/` ve `f` sorgu argümanlarını alır (`SPEC-search`): `sort:-size` `type:md` `in:work/` `size:>1mb` `updated:7d` `sync:modified`. Bozuk değer metin sayılmaz: girdide yeriyle `format_error` görünür, liste son geçerli haliyle kalır, taslak açılmaz. Argümanlı sorguda eşleşme yokken `Enter` taslak açmaz. `sort:` yoksa metinli sorgu fuzzy sırasındadır, boş sorgu `o` sırasındadır.

**Arama (fzf davranışı, girdi altta)**

| Tuş | Eylem |
|---|---|
| yazı | Sorguyu düzenler, liste anında süzülür; `d` gibi harfler yalnız metindir |
| `↑` `↓` | Aramada kalarak seçimi gez |
| `Ctrl-P` `Ctrl-N` | Geçmişte, şu anki metni (katlamalı alt dize) içeren önceki aramalar; en yeninin ötesinde yazılan metne döner, yazmak yeni süzgeç başlatır |
| `PgUp` `PgDn` | Önizlemeyi kaydır |
| `Enter` | Seçimi tutup Listeye dön; **eşleşme yoksa** sorgu ilk satır olacak şekilde kaydedilmemiş taslak (nvALT) |
| `Tab` | Tamamlama: tek aday doğrudan yazılır, çoksa açılır liste (bkz. "Girdi satırı") |
| `Ctrl-T` | Ad ↔ içerik modu |
| `←` `→` `Home` `End` `Delete` | İmleç ve silme; metnin sonunda `→` gölge öneriyi alır |
| `Esc` | Açık listeyi kapatır; yoksa süzgeç kalarak Listeye dön |

**Liste ve Arama ortak:** `Ctrl-O` yeni not · `Ctrl-R` ad/klasör/tip · `Ctrl-D` çöpe at · `Ctrl-S` senkron · `Alt+→` `Alt+←` satır ayrıntısı. Listede `Ctrl-P` `Ctrl-N` gezer, `Tab` ad ↔ içerik modudur.

**Girdi satırı (`InputLine`):** `f`, `/`, `F2`, ad istemi, editörde bul ve `a` satırı aynı saf bileşeni kullanır (`tui/input_line`, FTXUI'siz; çizim `tui/input_view`). REVIEW'daki `/` ve kaydet diyaloğu dışarıdadır.

| Özellik | Davranış |
|---|---|
| Gölge öneri (soluk) | İmleç metnin sonundayken. `f` `/`: önce geçmiş (tüm satır), sonra son sözcükle başlayan not adları. `a` `F2` ad istemi: klasörler, sonra adlar. Editörde bul: nottaki sözcükler. `→` alır |
| `Tab` | Adaylar: `klasör/`, not adları, `.cpp` gibi tipler (yolda yalnız uzantıyı değiştirir); `f` `/`'de ayrıca `sort:name\|size\|updated\|created`, `type:<tipler>`, `in:<klasörler>`, `size:`, `updated:`, `created:`, `origin:windows\|linux\|macos\|android\|unknown`, `sync:`. Tek aday doğrudan yazılır, çoksa fzf gibi liste: yazı süzer, `↑↓` gezer, `Enter` ekler, `Esc` kapatır. Boşluklu aday `f` `/`'de tırnaklanır |
| Renk (`f` `/`) | Geçerli argümanın anahtarı camgöbeği, değeri yeşil; bozuk değer `parse_query`'nin yerini bildirdiği aralıkta anında kırmızı; düz metin normal. Aralıklar bayt ofsetidir, UTF-8 imleci bölmez |
| Geçmiş (`f` `/`) | `Enter`le kullanılan arama `<state>/tui-state.toml`da `search_history`e girer: en yeni başta, yinelenmez, en çok 200, kayıt başına en çok 1 KiB, senkrona girmez. Bozuk sorgu ve taslak açan eşleşmesiz sorgu girmez. `KEY=değer`, `anahtar: değer` (argüman anahtarı hariç; `core` sır kuralı) ve 20 bayt üstü harf-rakam jetonu taşıyan ya da sözcüğü bir not adıyla birebir aynı olan sorgu yazılmaz |

`Ctrl+Space` yalnız editördedir. Editörde bul'da `Ctrl+P` `Ctrl+N` `↑` `↓` eşleşme gezmeye devam eder, geçmiş yoktur. Adaylar bellekteki listelerden gelir (`TuiState::input_sources`, notlar değişince ilk istekte kurulur); tuş başına depo okunmaz.

`Alt+ok` dizileri: `ESC [1;3C` / `ESC [1;3D` (xterm ailesi), `ESC [3C` / `ESC [3D`, `ESC f` / `ESC b` (macOS Terminal). rxvt'nin `ESC ESC [ C` dizisi tuş değildir: FTXUI 7.0.3 onu `ESC ESC` ve `[C` diye böler.

**Önizleme** (salt okunur, imleçsiz)

| Tuş | Eylem |
|---|---|
| `↑` `k`, `↓` `j` | Satır satır kaydır |
| `PgUp` `PgDn` | Sayfa sayfa kaydır |
| `g` `G` | Başa / sona (önek değil, tek tuş) |
| `c` | Notun özgün baytlarını kopyalar (Listedeki gibi) |
| `e` `Enter` | Görünen ilk satırdan Editöre geç |
| `E` | Harici editör |
| `/` | Not içi arama girdisi: canlı vurgu (`search::fold`: büyük/küçük harf ve Türkçe katlama, `I` = `ı` = `i`), `3/12`; `Enter` vurguyu bırakarak kapatır, `↑` `↓` gezer, `Esc` temizler |
| `n` `N` | Sonraki / önceki eşleşme |
| `←` `h` | Listeye dön |
| `Esc` | Vurgu varsa temizle, yoksa Listeye dön |
| `q` | Çık |
| `<` `>` `z` | Panel genişliği, yakınlaştırma |

Liste komutları içerik panelinde çalışmaz: `a d r s ?` ve `Ctrl-D` `Ctrl-R` `Ctrl-O` `Ctrl-S` `Ctrl-P` `Ctrl-N` `Tab` yok sayılır.

**Editör** (sağ panel, micro benzeri): tüm tuşlar düzenler; liste komutu çalışmaz, harfler metne girer, `←` yalnız imleci oynatır.

| Tuş | Eylem |
|---|---|
| `Ctrl+S` | Kaydet |
| `Ctrl+Q` | Önizlemeye dön; kaydedilmemiş değişiklikte Kaydet / At / Vazgeç sorar (varsayılan Kaydet). `n` taslağında boşsa sessiz kapanır, doluysa sorar, sonra iki panele döner |
| `Esc` | Yalnız açık arama/ad girdisini kapatır ya da seçimi kaldırır; tek başına çıkmaz, hiçbir şey atmaz |
| `Ctrl+Z` `Ctrl+Y` | Geri al / yinele; yazım kelime kelime birleşir, geçmiş 64 MiB ile sınırlı (en eski adım düşer) |
| `Ctrl+X` `Ctrl+C` `Ctrl+V` | Kes / kopyala / yapıştır; seçim yoksa satır. Sistem panosu (bkz. "Pano"); yapıştırma tek geri alma adımıdır |
| `Tab` `→` | Satır sonundaki gölge öneriyi alır; öneri yokken `Tab` sekme, `→` hareket |
| `Ctrl+Space` | Tamamlama listesi: yazı süzer, `↑↓` gezer, `Enter` ekler, `Esc` kapatır |
| `Ctrl+F` | Alttaki aynı girdi, aynı katlama ve vurgu: `Enter` `F3` `↓` `Ctrl+N` sonraki, `↑` `Ctrl+P` önceki, `Esc` kapatır ve vurguyu siler. Düzenleme eşleşmeleri düşürür; `F3` imleçten yeniden hesaplar |
| `Shift`+ok, `Shift+Home/End` | Seçim |
| `Home` `End` `PgUp` `PgDn` `Ctrl+Home/End` | Hareket; terminal göndermiyorsa tuş yoktur |
| `F1` | Yardım; metin yerinde kalır |
| `F9` | Ayarlar; kaydedilmemiş değişiklikte önce aynı soru |

`Shift+F3` yoktur: xterm ailesi `ESC [ 1 ; 2 R` gönderir, FTXUI 7.0.3 bunu imleç konumu yanıtı sayıp yutar; önceki eşleşme için `Ctrl+P`.

**Her odakta:** `F1` yardım · `F9` ayarlar. `Ctrl-C` Editör dışında acil çıkıştır, Editörde kopyalar; yardım editörün üstündeyken de çıkmaz. `q` Liste ve Önizlemede çıkar; Editörde ve hiçbir girdi satırında çıkmaz.

**Çizim:** terminale yalnız değişen hücre yazılır (`tui/frame_diff`, synchronized update ile sarılı); FTXUI'nin çıktı kancası `cmake/patches/` yamasındadır. Kuyruktaki girdi tek karede işlenir. `Ctrl+L` her odakta ekranı silip baştan çizer; harici editörden dönüş, askıdan dönüş ve boyut değişimi de öyle.

**Akış denetimi:** `Ctrl+Q` XON, `Ctrl+S` XOFF'tur. FTXUI 7.0.3 oturumda `IXON`'u kapatır ve çıkışta eski termios'u geri yükler (`app.cpp`); e2e başlangıçta `IXON` açık pty'de iki tuşun ulaştığını, oturumda `-ixon`, normal çıkış ve SIGTERM sonrası `ixon` olduğunu doğrular. Bu yüzden ek terminal kodu yoktur.

İçerik modunda liste not başına eşleşme sayısını gösterir; önizleme ilk eşleşmeye kayar ve eşleşmeleri vurgular.

## Klasör görünümü

Varsayılan liste klasör tabanlıdır (`src/tui/folder_tree.*`, saf). Ağaç `NoteStore::list` ve `folders`in bellekteki anlık görüntüsünden yalnız liste değişince kurulur (`O(not × derinlik)`); gezinme, klasöre girme ve çıkma depoya gitmez, klasör satırı önizleme okuması tetiklemez.

- **Satır:** önce klasörler, sonra notlar; `file_icons` ikonu (klasörde açık ya da kapalı ikon), ad kısa (`arşiv/`, `plan.md`), klasör adı `/` ile biter. `o` her grubun içinde uygulanır: klasörde ad, alt ağacın toplam boyutu, son güncellemesi, son oluşturması; bilinmeyen tarih azalan sırada sonda. Numara klasörle birlikte 1'den başlar, klasör başına yeniden başlar; numara genişliği ve `row_marker_column` en büyük numaradan gelir.
- **Ağaç:** `Space` klasörü yerinde açar: not ve alt klasörler altında, seviye başına iki hücre girintili (panelin en çok yarısı), her derinlikte; numara kendi klasöründe yeniden başlar. `→` `l` yine içeri girer (yazi), `←` `h` her zaman üst klasöre gider, açık satırı kapatmaz. Açık küme girip çıkınca kalır, `tui-state.toml`da `open_folders`; kalmayan klasörler yazarken düşer (taramada değil: senkron geri getirebilir). Seçim, sayfalama, toplamlar, fare satır eşlemesi, `x` ve toplu işlemler ağaç satırlarında çalışır; `p` ve `m`'nin ön değeri üst satırdaki klasördür. Liste yalnız değişimde kurulur (`O(görünür satır + açık klasörlerin çocukları)`), gezinme depo okumaz, çizim görünene orantılıdır: 20.000 satırlık açılış testi vardır.
- **`m` taşı:** seçim ya da satır için altta `Taşı:` girdisi, geçerli klasörle dolu; `Tab` yalnız klasörleri önerir (boşlar dahil, ağaçtan), çoksa fuzzy liste. `Enter` not için `NoteStore::move`, klasör için `rename_folder`; yazılan iç içe klasörler oluşur. Hiçbir şey ezilmez: hedefte ad doluysa hiçbir öğe taşınmaz, hata girdide kalır. Aynı klasör hiçbir şey yapmaz; kendi içine taşıma hata verir. `Esc` vazgeçer. Kopyadan farkı: `p` ad çakışmasında `-2` verir, `m` hata verir.
- **Üst satır:** çöp göstergesi, açık klasör ikonu, `notlar/work/sub`. Sığmazsa baştaki parçalar `…/` olur, bulunulan klasör görünür kalır. Sağ taraf için `kTopBarRightCells` (44) ayrılır. Parçaya tıklamak o klasöre gider, ayırıcı ve `…` tıklanmaz. Adlar `terminal_safe`ten geçer.
- **Sağ sütun:** klasörde `v` seçiminden bağımsız `12 öğe` (`yok` hariç). **Ayrıntı** (`i`): yol, öğe/not/klasör sayısı, toplam boyut, son güncelleme.
- **REVIEW:** klasör seçiliyken özet: öğe sayısı, toplam boyut, son güncelleme, alt ağacın en yeni beş notu (`FolderTree::kRecentNotes`). `→` klasörde içeri girdiği için panele fareyle gelinir.
- **Dönüş:** üst klasöre çıkışta çıkılan klasör seçili kalır. Süzgeçten dönüşte klasör ve seçim yerinde (`saved_spot_`); çöpten dönüşte klasör yerinde, seçim ilk satırda. Aramada `Esc` süzgeç kalarak Listeye döner (eski kural), ikinci `Esc` klasör görünümüne.
- **Silinen klasör:** yeniden taramada geçerli klasör gitmişse en yakın duran ataya düşülür. Kaydedilen ya da seçilen not başka klasördeyse (`select_note`, `reveal`) liste oraya gider.
- **Fare:** tıklama seçer, aynı satıra 400 ms içinde ikinci tıklama klasöre girer; `▸` `▾` okuna tıklama klasörü yerinde açar/kapatır, ikinci tıklama sayılmaz.
- **İpuçları:** klasör satırında ve boş klasörde `key_hints_folder`: `→ aç  ← üst  a ekle, ad/ = klasör  F2 ad değiştir  d çöpe at` 60 sütunda görünür.

**Klasör işlemleri**

| Tuş | Davranış |
|---|---|
| `a` `ad/` | `create_folder`; `sub/x.md` gerekli klasörleri oluşturur. Kökte klasörsüz ad köke gider (`default_folder` düz listede ve `n`'de kalır). Uzantı kuralı değişmez; klasör parçası slug'lanmaz. Var olan klasör adı onu seçer, "zaten var" der, yazmaz |
| `F2` `r` `Ctrl-R` | `rename_folder`; hedef doluysa ya da kendi içineyse hata girdide kalır. Klasör adındaki nokta uzantı değildir |
| `d` `Ctrl-D` | Notsuz klasör onaysız kaldırılır (boş alt klasörleriyle). Notlu klasör onay ister, sayıları söyler, varsayılan Vazgeç. Çöp not başına olduğu için `remove_folder(force)` kullanılmaz (notları kalıcı siler): `ListActions` notları tek tek `trash`ler, sonra boşalan klasörü `remove_folder(false)` ile kaldırır. Arada gelen not `NotEmpty` verir, klasör ve o not kalır, atılanlar `u` ile döner |
| `u` | Son adımın tamamı: klasörler (boş alt klasörler dahil) ve notlar; ad doluysa çöp kuralı `-2` |
| `D` | Kalıcı, onaylı (sayılar, varsayılan Vazgeç): `remove_folder(force=true)` |
| `c` | Klasörün yolunu kopyalar |

## Çoklu seçim ve yank

Yazi'den esinli; tuş kararı `TuiState`, yan etki `ListActions` (`main` yalnız `Ctrl+A` eşlemesini bilir).

- **Seçim:** `x` satırı (not ya da klasör) seçer ve aşağı iner, `X` temizler, `Ctrl+A` görünen tüm satırları seçer; arama sonuçlarında tam yolla çalışır. Satırda `*` ve sarı ad; renk tek sinyal değildir. Klasör değişince kalır, alt satırda `seçili: 3`. Yeniden taramada kaybolan öğe düşer. Çöpün ayrı seçimi vardır (ayrı ad uzayı), çöpten çıkınca silinir.
- **Hedef:** `d` `D` `y` `m` (taşıma, bkz. "Ağaç") seçim doluysa onu, değilse geçerli satırı işler. Seçili klasörün içinde ayrıca seçilmiş öğe tek sayılır, ikinci kez işlenip hata vermez.
- **`d`:** onaysız çöpe; seçimde notlu klasör varsa toplam sayıyla sorar, varsayılan Vazgeç (tek klasör kuralı). Tek `u` adımın tamamını geri alır.
- **`D`:** kalıcı, toplam öğe ve not sayısıyla sorar, varsayılan Vazgeç.
- **`y` `p`:** `y` kopya için işaretler, seçim tüketilir, alt satırda `kopya: 2`. `p` geçerli klasöre yapıştırır, düz listede klasör ister; kaynak yerinde kalır. Not kopyası baytları okuyup `create_exact`, klasör kopyası `NoteStore::copy_folder` (boş alt klasörleriyle, toplu işlem). Hiçbir şey ezilmez: dolu ad `-2`, `-3` (`a.tar.gz` -> `a.tar-2.gz`; klasör `iş-2`), her biri raporlanır. Klasör kendi içine kopyalanmaz. `Esc` bekleyen yank'ten vazgeçer. `c` sistem panosudur, yank'e karışmaz. Uzantısız not kopyalanamaz (`create_exact` tip ister), hata raporlanır, kaynak yerinde kalır.
- **Kısmi hata:** kalan öğeler sürer. Sorunsuz işlem alt satırda tek ileti; hata ya da ad değişimi varsa öğe öğe küçük pencere (`2 tamam, 1 başarısız`), sıradaki tuş kapatır. `u` da aynıdır: geri gelmeyen not raporlanır, çöpte artık yoksa adımı kilitlemez.
- **Çöp:** `x` `X` `Ctrl+A` ile `u` geri getirir, `D` kalıcı siler (sayıyla, varsayılan Vazgeç).
- **Senkron:** değişiklik yapan her işlem sonda bir kez `notify_changed` çağırır.

## Yeni not

- `a` notu adıyla başlatır: listenin üstünde, liste panelinin içinde boş bir satır açılır, tuşlar oraya gider (alt satırda yalnız ipuçları). Liste satırın ve seçili notun görüneceği kadar kayar, numara sütunu hizasını korur (satırın numarası boştur, yol aynı hücrede başlar). `Enter` notu **hemen ve boş** oluşturur, sağ panel EDIT'te açılır; boş bırakılsa da kalır. Ad `klasör/ad.uzantı` olabilir, klasör görünümünde geçerli klasöre göredir; düz listede klasör yoksa `n`'deki gibi seçili notun klasörü, o da yoksa `default_folder`. Uzantı yoksa `default_type` (`detect` yok: daha içerik yok). `-2` eklenmez: dolu ad hatadır. **Süzgeç açıkken** satır süzgeçten etkilenmez; yeni not süzgeçten geçiyorsa süzgeç kalır, geçmiyorsa süzgeç temizlenir ki not görünsün (`select_note` kuralı).
- `n` ya da `Ctrl-O` önce ad sormadan, harici editör olmadan boş editörü tüm terminalde açar: liste ve üst çubuk gizlenir (panel yakınlaştırması), üst satır "Yeni not" der. **İlk kayda dek dosya yoktur.**
- İlk `Ctrl+S` altta tek satırlık ad girdisi açar, ilk satırın slug'ı ve `detect` uzantısıyla dolu gelir. `Enter` kaydeder ve düzenlemede kalır; `Esc` hiçbir şey yazmadan düzenlemeye döner. Boş taslakta `Ctrl+S` ne sorar ne dosya oluşturur. Geçersiz yol ya da yazma hatası girdide görünür, girdi açık kalır.
- Ad `klasör/ad.uzantı` olabilir; klasör yoksa seçili notun klasörü, o da yoksa `default_folder` (girdinin etiketinde yazar); açık uzantı kazanır (bkz. "Uzantı kuralı"), uzantı yoksa `detect`. Çakışmada mevcut `-2` kuralı. Girdi boşaltılırsa zaman damgalı ad.
- **Sır koruması:** `env`, `properties`, `ini`, `toml`, `yaml` içerikte öneri ilk satırın yalnız anahtarından gelir (`API_TOKEN=gizli` → `api-token.env`): değer dosya adına ve senkrona girmesin. Sinyal çıkışında taslağa verilen ad da aynı kuraldandır.
- `Ctrl+Q`: boş taslak sessiz kapanır; dolu taslakta Kaydet / At / Vazgeç (varsayılan Kaydet; Kaydet ad girdisini açar). Kapanınca normal iki panele Liste odağıyla dönülür, kaydedilen not seçilidir.
- Aramada eşleşme yokken `Enter`: sorgu ilk satır olan kaydedilmemiş taslak; ad ilk kayıtta içerikten (nvALT).

## Uzantı kuralı

Ad veren her yolda (`a`, `n` ad istemi, `F2`, `r`, `note -n`, `note mv`, GUI kaydet/taşı formu) **yazılan uzantı değişmez, yerine başkası konmaz, üstüne eklenmez**: `deneme.cpp`, `deneme.xyz` (`types` dışında da), `a.tar.gz`, `notlar.v2`. Uzantı son noktadan sonrasıdır (`core::split_typed_name`); baştaki ya da sondaki nokta ve boşluk taşıyan son parça uzantı değildir (`v1.2 son`). Uzantı yazılmadıysa: `a` `default_type`, `n` ve `note -n` `detect`, `r` ve GUI formu Tip alanı, `F2` ve `note mv` hiçbiri (yazıldığı gibi).

- **Boşluk ve slug (eski kural aynen):** `a` ve `n` gövdeyi slug'lar (`Toplantı Notu.CPP` -> `toplanti-notu.CPP`: Türkçe harf, küçük harf, boşluk `-`; noktalar parça ayırıcı olarak kalır, `Sürüm 1.2.txt` -> `surum-1.2.txt`). `F2`, `r`, `note -n`, `note mv` yazılanı aynen alır, boşluk ve büyük harf kalır. Uzantıya hiçbir yolda dokunulmaz, büyük/küçük harfi korunur.
- **Yazılan uzantı Tip alanını ve `-t`'yi geçer:** `r` ve GUI formunda adda uzantı varsa Tip alanı kullanılmaz; `note -n a.cpp -t py` `a.cpp` yazar. Çok noktalı notun `r` ve GUI formu adın tamamıyla açılır.
- **Gizli ad:** `.env` gibi ad reddedilir (`hidden`); eskiden `n` onu `env.md` yapardı.
- `type` özelliği ve vurgu son uzantıdan türer (`a.tar.gz`: `gz`, düz metin; `kod.CPP`: `CPP`, cpp vurgusu).

## Ad/klasör/tip diyaloğu

- `r` / `Ctrl-R` küçük çerçeveli diyalog: **Ad**, **Klasör** (yazarken mevcut klasörler fuzzy önerilir, yeni ad yazılabilir), **Tip** (`←`/`→` ile `types` listesi ya da uzantı yazılır). Kaydet = `NoteStore::move` (hedef varsa hata, ezme yok). `Esc` iptal.
- Harici editör (`E`), FTXUI terminali bırakarak (`WithRestoredIO`) çalışır; kapanınca ekran geri gelir ve liste yenilenir.

## Gömülü editör

Tampon `EditorBuffer` (saf, FTXUI bilmez), tuş ve kayıt kararları `EditorSession`. FTXUI `Input` geri alma, seçim, arama ve kaydırma sunmadığı için kullanılmadı; çizim `editor_view`'dadır.

- **Bayt koruma:** tampon dosyanın baytlarıdır, satır sonları normalleştirilmez. CRLF ve karışık satır sonu düzenlenmeyen satırlarda aynen kalır; yeni satır dosyada CRLF varsa CRLF, yoksa LF'dir. BOM tamponda görünmez, kayıtta geri konur. Sona satır sonu eklenmez. İçeriği diskle aynı tampon yazılmaz (yazıp geri silmek dahil).
- **Görünüm (yalnız Editör; REVIEW `cat` gibi kalır):** satır numarası sütunu (genişlik satır sayısına göre, en az 2 basamak + boşluk; geçerli satır kalın, diğerleri soluk, sarılan satırın devamı boş), geçerli satır arka planı (`GrayDark`), gerçek terminal imleci (arama/soru tuşları alırken gizli), dosya sonunun ötesinde soluk `~`. Numara sütunu metin değildir: `EditorSession::columns()` hariç tutar, sarma ve imleç metin sütunlarıyla hesaplanır. Alt iki satır: durum çubuğu (dosya ya da `[yeni not]`, `*`, tip, `UTF-8`, `LF`/`CRLF`, `BOM`, satır:sütun, toplam satır; uzun adın sonu görünür) ve ileti + kısayollar (arama ve ad girdisi kısayolların yerine geçer). 60×15, `z` ve `n` görünümünde aynıdır.
- **Karakter:** imleç grapheme sınırındadır; çok baytlı karakter ya da birleşen işaret bölünmez, CRLF tek adımdır. Genişlik FTXUI'nin ölçüsüdür. Kontrol baytları (C0, DEL, C1) ekranda `U+FFFD`, sekme sonraki 4'lü durağa boşluktur; terminale ham bayt gitmez, dosyadaki bayt değişmez.
- **Salt okunur:** ikili (`looks_binary`), NUL içeren, geçersiz UTF-8 ve `tuning.max_note_bytes`'tan (10 MiB) büyük not. Neden durum çubuğunda görünür; kaydırma, arama, kopyalama çalışır; hiçbir yol yazmaz. Gösterilen metin arındırılmış kopyadır, büyük notta başı.
- **Boyut sınırı gerekçesi** (Release, 120×40, tuş + yerleşim + çizim): 10 MiB satırlı notta en kötü 2,8 ms; 10 MiB tek satırda medyan 32-45 ms (makine yüküne göre; yük altında tepe 70 ms'e çıkar); canlı not içi arama 10 MiB'te 34-42 ms; sınır 50 ms (SPEC.md §10/3). Daha büyüğünde tek satır bütçeyi aşacağından mevcut `max_note_bytes` sınırı olarak kaldı.
- **Kaydetme** yalnız `NoteStore::write` (atomik, mtime denetimli). Başarıda `notify_changed`, liste ve içerik indeksi yenilenir. Hatada tampon kalır, hata durum çubuğundadır, soru ekranından çıkılmaz.
- **Çakışma** (`ModifiedExternally`): Benimkini kopya olarak kaydet (varsayılan; `ad (copy).uzantı`, baytlar aynı, editör kopyaya geçer) / Disktekini yükle / Üstüne yaz / Vazgeç.
- **Dış değişiklik:** yeniden tarama ve senkron kirli tampona dokunmaz, durum çubuğu uyarır. Temiz tampon yenilenir, imleç aynı satır/sütunda kalır. Not silinmişse temiz tampon kapanır; kirli tamponda metin kalır, `Ctrl+S` aynı adla yeni not yazar.
- **Çıkış:** editörden `q` ve `Ctrl-C` ile çıkılamaz. SIGTERM, SIGHUP ve dışarıdan SIGINT'te kirli metin normal yoldan yazılır (taslak oluşturulur, çakışmada kopya, olmazsa geçici dosya; sonuç stderr'e), süreç 128+N ile biter. SIGKILL ve kilitlenme kapsam dışıdır (takas dosyası yok).
- **Yapıştırma:** FTXUI 7.0.3 bracketed paste sunmaz. Terminalden yapıştırma (`Ctrl+Shift+V`) tuş olayı olarak gelir: editörde metne girer, Listede harfler kısayol olarak çalışır (bilinen sınır). `Ctrl+V` sistem panosunu okur (bkz. "Pano").

## Pano

- **Arka uçlar** (`src/tui/clipboard.*`, kitaplık yok): Windows Win32 API; `WAYLAND_DISPLAY` varsa `wl-copy`/`wl-paste --no-newline`, `DISPLAY` varsa `xclip -selection clipboard`, yoksa `xsel --clipboard`; macOS `pbcopy`/`pbpaste`. Kabuksuz, veri stdin'den, 1,5 sn süre sınırı (takılan araç UI'yı en çok bu kadar bekletir, sonra öldürülür). Çocuk ortamından `SSL_CERT_*`, `LD_*`, AppImage değişkenleri ve senkron token'ı düşer. Çatallanıp seçimi tutan araç beklenmez: stdout boş aygıttır, ana sürecin çıkışı yeterlidir.
- **Yedek:** araç yoksa ya da başarısızsa kopya OSC 52 ile terminale gider; base64'ten önce en çok 75.000 bayt, üstünde ileti ve yalnız iç pano. Araç başarısız olduysa sistem panosu eskidir: sonraki `Ctrl+V` iç panoyu seçer. Araç yokken `Ctrl+V` iç panoyu yapıştırır ve `Ctrl+Shift+V`'yi söyler. Pano hatası çökertmez, metin kaybetmez.
- **Reddedilen veri:** NUL içeren metin kopyalanmaz; NUL ya da geçersiz UTF-8 içeren ve 32 MiB'ten büyük pano yapıştırılmaz, metin değişmez, neden söylenir.
- **REVIEW fare seçimi:** sürükleme seçer, ters renkli gösterir, bırakınca kopyalar. Uçlar mantıksal konumdur: sarma ve gutter kopyaya girmez, satır sonları dosyadaki gibidir (CRLF). İşaretçinin altındaki karakter seçime dahildir. Panel kenarını geçen fare en yakın hücreye kenetlenir; sürüklerken tekerlek kaydırır, çapa yerinde kalır, uç aynı hücrenin yeni metnine gelir. Tıklama seçmez ve kopyalamaz. Sol tuş bırakılmadan başka olay gelirse (Release kaybı) sürükleme biter ve kopyalar. Geçersiz UTF-8 notta seçim kopyalanmaz, neden söylenir. Tuş ve yeni tıklama seçimi kaldırır; bölücü sürükleme ve liste tıklaması aynı kalır.

## REVIEW biçimli görünüm

`src/tui/markdown_view.*` (md4c, `render_markdown`), `src/tui/data_view.cpp` (`render_csv`, `render_json`), `src/tui/styled_view.*` (sarma, tablo yerleşimi): kaynak baytlar + genişlik -> biçimli satırlar. Saf: ağ, dosya, terminal kaçışı yok; FTXUI'ye yalnız `view.cpp` boyarken değer. Uzantı seçer: `md`/`markdown`, `csv`, `json`; diğer notlarda yalnız ham metin vardır, `R` etkisizdir, etiket `REVIEW` kalır.

- **Markdown:** başlık düzey işaretiyle (`#`, `##` soluk; her düzey ayrı biçim), kalın, eğik, üstü çizili, satır içi kod, kuşatılmış kod (`syntax` tokenizer'ı dil etiketinden; `python`, `javascript` gibi tam adlar da, bilinmeyen dil düz camgöbeği), alıntı `│` ile, iç içe sıralı ve işaretli liste, görev listesi `[x]` `[ ]`, sarılan hücreli tablo, yatay çizgi, bağlantı metin + soluk URL (terminal hyperlink kaçışı yok), görsel `[görsel: alt]`, HTML yazıldığı gibi. Renkler 16 ANSI setinden, aynı `token_styled` paleti.
- **CSV:** ayırıcı `detect` kuralıyla (`detect::table_delimiter`: ilk 200 dolu satır); tırnaklı alan, `""`, tırnak içi satır sonu boşluğa iner. İlk satır başlık. Tutarlı tablo değilse ham metin + not.
- **JSON:** katı RFC 8259 denetiminden geçerse girintilenir, `syntax` JSON renkleriyle; geçersizse ham metin + not. Derinlik sınırı `kMaxJsonDepth` (128).
- **Geri düşüş:** görünüm olmadıysa (geçersiz, tablo değil, `kMaxRenderedViewBytes` 256 KiB üstü) ham vurgulu metin ve toplamlar satırında soluk neden. Not içerik satırı değildir: kaydırma, fare ve arama eşlemesi bozulmaz. Etiket `ham` der.
- **Güvenilmez içerik:** çıkan her metin `terminal_safe`ten geçer (C0, C1, DEL, bidi, ESC yok). Süre girdiyle doğrusal; sınırlar görünürdür: `kMaxViewLines` (100.000), `kMaxTableRows` (5.000), `kMaxTableColumns` (128), sığmayan sütun ve kesilen satır soluk satırla söylenir. İç içelik öneki panelin en çok yarısını alır, ek derinlik girinti katmaz.
- **`R`:** görünüm / ham, oturum boyunca tüm notlarda; `REVIEW · görünüm`, `REVIEW · ham` etiketi (Türkçe katalogda çerçeve adı `İNCELEME`). EDIT her zaman ham metindir. Kaydırma geçişte aynı kaynak bloğunda kalır.
- **Arama (`/`, `n`, `N`):** gösterilen metinde çalışır; biçimli görünümde `**` bulunmaz, `kalın` bulunur. Eşleşme görüntü satırı içindedir: sarmanın böldüğü sözcük bulunmaz. İçerik aramasının (`Tab`, `f`) satır vurguları ham satıra aittir, biçimli görünümde boyanmaz; önizleme yine eşleşen bloğa kayar.
- **Kopya:** fare seçimi gösterilen metni kopyalar (satırlar `\n` ile, tablo kenarları ve liste imleri dahil; geçersiz UTF-8 not da arındırılmış haliyle kopyalanır). `c` her zaman notun özgün baytlarını kopyalar.
- **`e` / `Enter`:** üstte görünen bloğun kaynak satırında EDIT açılır: `RenderedView::source_lines` görüntü satırı -> kaynak satırı eşlemesidir (kod ve tablo satırı başına, diğerleri blok başına). md4c konum vermez; metin geri çağrısının gösterdiği adresten bulunur, metni olmayan blok (çizgi) sonraki dolu satırdır. EDIT'ten dönüş o bloğa iner.
- **Önbellek:** `PreviewCache` not sürümü ve genişlik başına bir kez üretir; hızlı gezinmede `throttled` okumayı ertelediği için görünüm de üretilmez. Genişlik değişince yeniden sarılır; art arda değişimlerde önceki yerleşim kalır, `kResizeSettleMs` (120 ms) durunca üretilir, sakin kullanıcının tek değişimi beklemez. Üretim UI iş parçacığındadır, bu yüzden boyut sınırı 50 ms bütçesinden gelir (ölçüm aşağıda). `main.cpp` değişmedi: eşleme `throttled` içinde modele bildirilir.

Ölçüm (T69, Release, 80 sütun, üç koşunun en kötüsü, sınır boyutunda girdi): 2 MiB'te karışık md 155 ms, bağlantı 144, kod 120, JSON 86 ile 50 ms bütçesini aştı; 512 KiB'te hepsi 33 ms altına indi ama `[` yığını 85 ms kaldı. 256 KiB'te en kötü `[` yığını 45 ms, JSON 12, uçtan uca genişlik değişimi (karışık md) 9, diğerleri 10 ms ve altı. Üretimi arka iş parçacığına almak yerine sınır düşürüldü: daha büyük not ham metinle zaten okunur, görünüm iptali ve eski sonuç yarışı eklenmez.

## Tamamlama ve yazım denetimi

- **Gölge öneri:** yalnız satır sonunda, yazılan her harften sonra; önek en az 2 bayt. Önce bu nottan imlece en yakın sözcük, sonra diğer notlardan en sık geçen. Her tuş bir önceki öneriyi düşürür, hareket ve `Esc` yeni üretmez. Soluk çizilir, panele kırpılır, sarmaya ve bayta girmez. Bu not imleç çevresinde ±256 KiB taranır: tuş süresi not boyutundan bağımsızdır, daha uzaktaki sözcük önerilmez.
- **Diğer notlar:** `WordTableBuilder` notları işçide okur, tablo yalnız bellektedir (en çok 200.000 sözcük; not başına 2 MiB, toplam 64 MiB), indeks dosyası yoktur. İlk editör açılışına dek hiçbir şey okunmaz, kayıttan 1 sn sonra yeniden kurulur. `ContentIndex` kullanılmadı: metni özeldir ve UI iş parçacığına aittir.
- **Sır koruması:** Yalnız satır anahtarları görünür (ad kuralıyla aynı `core::secret_line_key`; değer ve yorum öneriye, tabloya ve başka nota girmez): `env`, `properties`, `ini`, `toml`, `yaml` ve betik ya da yapılandırma tipleri `sh`, `bash`, `zsh`, `fish`, `ps1`, `bat`, `cmd`, `json`, `jsonc`, `json5`, `conf`, `cfg`, `tf`, `xml`, `dockerfile`. Diğer tiplerde düzyazı önerilir ama `KEY=değer` ve `export KEY=değer` satırı yalnız anahtarını verir. Satırı pencereye sığmayan sözcük önerilmez; bir öneri için en çok 64 aday reddedilir: tuş süresi sınırlı kalır.
- **Liste (`Ctrl+Space`):** adaylar bu notun penceresi ve tablo, tekil, en çok 5.000; `search::fuzzy_match` ile süzülür, ilk 8'i görünür. Seçim önekin yerine tek geri alma adımıyla girer. Terminal Ctrl+Space'i NUL gönderir; FTXUI 7.0.3'te `Event::Custom` de `Special({0})`dır ve olaylar yalnız baytla karşılaştırılır, bu yüzden arka plan çizimleri kendi olayını (`redraw_event`) gönderir.
- **Yazım denetimi:** yalnız `md` ve `txt`; sözlük dilden (`tr` -> `tr_TR`, diğerleri `en_US`), config yok. `hunspell -a -i utf-8 -d <sözlük>`, `LC_ALL=C.UTF-8`. Satırlar `^` önekiyle gider (`*sözcük` ispell komutu olup kişisel sözlüğe yazardı). Çizim önbelleğe bakar, bilinmeyen satırı kuyruğa atar: 250 ms boşta kalınca işçi sorar, yanıt çizimi tetikler. Süreç ilk istekte başlar, nesneyle ölür. Program yoksa, başlamazsa, 2 sn yanıt vermezse ya da ölürse oturum boyunca kapalıdır, yeniden denenmez; `F1` bunu söyler. 8 KiB üstü satır denetlenmez. Windows'ta kapalıdır (`ChildProcess` POSIX). İçerik makineden çıkmaz.

## Sözdizimi vurgusu

REVIEW ve EDIT'te; tablo güdümlü, bağımlılıksız satır ayrıştırıcı (`src/tui/syntax.*`). Dil uzantıdan (bilinmeyen tip, `txt`, `csv` düz), kaydedilmemiş taslakta canlı `detect`ten gelir: metnin 1/32'si kadar değişiklikte bir yeniden algılama, yani küçük metinde her tuş, büyük metinde seyrek.

- **Diller:** java, cpp, c, py, sh, js, ts, go, rust, sql, json, css, html, xml, yaml, toml, ini, env, properties, dockerfile, makefile, md. Yorum, dizge, sayı, anahtar sözcük, tip/sabit; env/properties/ini/yaml/toml'da anahtar ve değer; xml/html'de etiket; md'de başlık, kod, vurgu.
- **Durum:** çok satırlı yorum ve dizge satırlar arasında sürer; `SyntaxStateCache` satır çıkış durumunu yalnız istenen satıra dek hesaplar, düzenleme o satırdan sonrasını düşürür (`EditorBuffer::take_changed_line`). Boyama yalnız görünen satırdadır. Bilinen sınır: heredoc ve yaml blok metni ayrı tanınmaz, satır satır boyanır.
- **Eşikler:** `kMaxHighlightBytes` 2 MiB üstü not ve `kMaxHighlightLineBytes` 64 KiB üstü satır düz metindir (satır durumu olduğu gibi geçirir). Tarama her dalda en az bir bayt tüketir ve geri dönmez; kapanışı olmayan ayraç bir daha aranmaz: süre doğrusaldır.
- **Renkler:** 16 ANSI. Yorum yeşil, dizge/değer eflatun, sayı/sabit açık kırmızı, tip/kod camgöbeği, anahtar sözcük/anahtar/etiket açık mavi, başlık kalın açık mavi, vurgu kalın. Sarı aramaya ayrılmıştır.
- **Değişmezler:** bayt, sarma, imleç hesabı ve kontrol baytı arındırması değişmez (renk yalnız hücrenin ilk baytına bakar); arama ve seçim sözdizimi renginin önüne geçer.

## Dosya ikonları

`src/core/file_icons.*` saf tablosu: uzantı ya da `Dockerfile`/`Makefile` adı → Nerd Font kod noktası, renk (16 ANSI); ayrıca
varsayılan dosya, açık/kapalı klasör, çöp. Eşleşme yolun son parçasına bakar, büyük/küçük harf duyarsızdır. Klasör satırı
ağaçtaki durumuna göre açık ya da kapalı, üst satırdaki yol açık klasör ikonunu kullanır; REVIEW çerçevesi notun glifini taşır.

| `icons` | TUI |
|---|---|
| `nerd` (varsayılan) | Nerd Font glifi, ardında boş hücreyle. Mono olmayan Nerd Font (Konsole'da `MesloLGS Nerd Font`: glif en çok 1,45 hücre) boş hücreye taşarak tam boyutunda çizilir; `Nerd Font Mono` tek hücrede küçük kalır. Terminal hücresinde font boyutu değiştirilemez: ikon boyutu terminal fontunu izler. Terminalin fontu sorulamaz, **Nerd Font yoksa glifler kutu görünür: `icons off` ayarlayın** |
| `off` | İkon sütunu yok |

Eski `auto` değeri `nerd` okunur ve `note config set` onu kabul eder; düz Unicode yedek kaldırıldı.

Satır: ` 1 ▸ <glif>  work/`, not ` 2   <glif>  plan.md`. İkon oktan sonra, adın önündedir ve taşma hücresi ile boşluğuyla 3 hücre tutar; ikon
tıklaması yalnız satırı seçer. Sağ sütun bu 3 hücreyi hesaba katar, ad satırı (`a`) aynı hizaya kayar. Seçili satırda renk de
ters çevrilir. Çöp göstergesi ve çöp başlığı çöp ikonuyla başlar.

Kod noktaları Nerd Fonts v3.5.1'in `SymbolsNerdFontMono-Regular.ttf` cmap'inde doğrulandı; yalnız Material Design ve Seti
kümelerinden seçildi. TUI font gömmez: terminalde font kullanıcınındır, uygulama yalnız kod noktası yazar.

Tam Symbols Nerd Font gömülmez: arşiv `LICENSE`'ı ve `fonts.json` MIT der, kök `LICENSE` OFL-1.1 der, dosyada OFL, CC BY ve
markalı kümeler de vardır; birleşik dosyanın lisansı üstten belirlenemiyor. Kullanıcı kararı: GUI yalnız tablodaki 30 glifi
taşır (SPEC-gui "Dosya ikonları"); kaynak ve metinler `resources/fonts/LICENSE-NoteappIcons.txt`. `note font install` yoktur.

## Ayarlar ekranı (`,` `F9`)

SPEC-core config alanlarının formu: dil (döngü), notes_dir, editör, varsayılan tip (döngü), varsayılan klasör, tipler,
dosya ikonları (döngü),
senkron açık/kapalı, remote, dal, idle süresi, yazar adı/e-posta; remote https ise **token** alanı (maskeli;
"kasada kayıtlı ✓" göstergesi, Kaydet/Sil). Kaydet → doğrula → `config.toml` atomik yazılır → uygulanır
(dil anında; notes_dir değişirse liste yeniden yüklenir; senkron açıldıysa `enable` akışı, hata satır içinde).
Vazgeç → değişiklikler atılır. AutoSync yalnız `sync_restart_needed` (etkin/remote/dal/idle/notes_dir) true ise yeniden
kurulur; önce `stop(exit_timeout)` ile düzgün durdurulur (T36). Açılışta config yükleme uyarıları alt çubuk iletisinde
görünür; `Enter` ile dış editör `NoteStore::edit_in_temp_file`'dan geçer (G4).
  - Sapma (T23): `Kaydet` düğme satırında `Enter` ile kaydedilir, `Esc` vazgeçer (değişiklik varsa "Değişiklikler atılsın mı?" onayı, varsayılan "Vazgeç"). Sapma (T28): token satırı 12 config satırının ardında (Kaydet düğmesinden önce) ve yalnız https remote iken görünür; `Enter` yazılan token'ı kasaya kaydeder, `Ctrl-D` siler, `Kaydet` yazılmış ama kaydedilmemiş token'ı da `enable`'dan önce kasaya yazar. `enable` akışı senkron açılırken ya da açıkken remote değişince çalışır; origin farklıysa "değiştirilsin mi?" onayı (varsayılan "Vazgeç"), hata formda satır içi. Kullanıcı `config.toml`'u bozuksa kaydedilmez (`note config set` ile aynı kural).

## Yenileme ve senkron
- Liste: harici editör dönüşünde, kayıtta, senkron çekme sonrası ve `rescan_seconds` (defaults.toml, 5 sn) aralıkla
  (yalnız stat; değişiklik yoksa yeniden çizim yok; değişiklik tuşa basılmadan ekrana gelir).
- `AutoSync`: açılışta başlar, her kayıtta `notify_changed`, çıkışta `stop(exit_timeout)`; durum `screen.Post` ile.

## Kabul kriterleri

1. Ekranı metne render eden testler: ilk liste, filtrelenmiş liste (vurgu), içerik modu, kaydet diyaloğu, ayarlar, küçük pencere.
2. **(değişti 2026-11-01)** Durum modeli (`TuiState`, saf) testleri: odak başına tuş → durum geçişleri; Aramada eşleşme yokken Enter'ın taslak akışını tetiklemesi (Listede tetiklememesi); Esc'in odaklardaki mantığı.
3. **(değişti 2026-11-01)** Yeni not: `n` ad sormadan tam terminal açar; ilk `Ctrl+S` slug + `detect` ile dolu ad girdisi açar, `Enter` kaydedip düzenlemede kalır, `Esc` yazmaz; açık uzantı kazanır, kayıttan önce dosya yok, dolu taslak onaysız atılmaz, kapanınca iki panel ve seçili not.
4. **(değişti 2026-11-01)** Kalıcı silme (`D`, çöpten silme) yalnız onayla, varsayılan seçim "Vazgeç"; `d` çöpe onaysız atar, `u` geri alır.
5. 5.000 notla açılış ≤ 500 ms, tuş başına filtre ≤ 50 ms (SPEC.md §10).
6. Ayarlar ekranında kaydedilen değer `config.toml`'da ve `note config get` çıktısında görünür.
7. Açılış odağı Listedir; harf kısayolları Listede çalışır, Aramada yalnız metindir, diyalog/ayar/yardım/ad satırı açıkken tetiklenmez; `ı` ve bilinmeyen harfler yok sayılır.
8. **(değişti 2026-11-01)** `a`: ad satırı liste panelinin ilk satırı, numara sütunu hizalı, alt satır girdi değil; `Enter` boş notu oluşturup EDIT'te açar, boş kalsa da durur; `Esc` ve boş girdi hiçbir şey yazmaz; geçersiz ve dolu ad satır içinde hata, metin kalır. Uzantı kuralı her ad yolunda test edilir.
9. **(değişti 2026-11-01)** Önizlemede liste komutları yok sayılır, `q` çıkar; Editörde harfler metne girer, `Ctrl-C` kopyalar, çıkmaz, `Ctrl+Q` ayrılır, `Esc` ayrılmaz; Listede boş süzgeçte `Esc` çıkmaz; `F1`, `F2` çalışır. Listede seçim değişince önizleme aynı karede güncellenir.
10. Odaklı panel ekranda ayırt edilir; kısayol çubuğu odağa göre değişir ve 60 sütunda kelime ortasında kesilmez (TR/EN).
11. Editörde `Ctrl+Q` ve `F9` kaydedilmemiş değişikliği sorar (varsayılan Kaydet); başarısız yazma tamponu korur; çakışma seçenekleri ve varsayılan kopya; kirli tampon yeniden taramada değişmez; salt-okunur not ve temiz tampon yazılmaz.
12. CRLF, BOM, kontrol baytı ve sondaki satır sonu yokluğu düzenleme sonrası bayt bayt korunur; rastgele düzenleme dizisi geri alınınca özgün baytlara döner.
13. **(değişti 2026-11-01)** Tuş başına ≤ 50 ms: 1 MiB, eşik ve 10 MiB, satırlı ve tek satır, vurgu ve numara sütunu dahil (Release, `editor latency`).
14. Gerçek terminal: `tests/tui/e2e_editor.sh` (tmux + podman): Türkçe yazım, kayıt, SIGKILL sonrası baytlar, SIGTERM/SIGHUP'ta kirli metin ve taslak, `n` akışı, arama, not içi arama, `<` `>` `z`, fare sürükleme ve tıklama, dış değişiklik.
15. Not içi arama Önizlemede ve Editörde Türkçe katlar (`I`/`ı`); düzenleme eşleşmeleri güvenle düşürür. Bölücü ve `<` `>` sınırları, yakınlaştırma, girdi satırlarında harflerin eylem yapmaması.
16. Kip çerçevesi: etiket ve renk kip başına (render testi). Sözdizimi: dil başına aralıklar, satırlar arası durum, düzenleme geçersizleştirmesi, kötü niyetli girdi (kapanmamış dizge, dev satır, geçersiz UTF-8) çökmez ve doğrusal kalır; arama sözdiziminin önüne geçer.
17. Editör görünümü: numara sütunu, boş devam satırı, gerçek imleç konumu, geçerli satır, `~`, durum çubuğu alanları 80×24 ve 60×15'te, `z` ve `n` görünümünde; REVIEW'da numara, imleç ve `~` yok.
18. Pano: `tests/tui/clipboard_test.cpp` sahte araçlarla (stdin, ortam, NUL ret, büyük veri, takılan araç, çatallanan araç, EPIPE); OSC 52 sınırı; `Ctrl+V` tek geri alma adımı; REVIEW sürüklemesinde sarma, CRLF, ters yön, kenar kenetleme, tekerlek, Release kaybı.
19. Tamamlama: `tests/tui/completion_test.cpp`; sır tiplerinde değer önerilmez. Yazım: sahte hunspell (askı, ölüm, yeniden başlamama, çizimde alt çizgi) ve gerçek sözlükler `tests/tui/hunspell_real.sh` (`tr_TR`, `en_US`). Tuş başına ≤ 50 ms, öneri ve yazım önbelleği dahil, 1 ve 10 MiB (`editor latency: completion`).
20. Liste: `tests/tui/list_ui_test.cpp`: argümanlar, bozuk argümanın yeri, `f`, `d` `u` `D` `T`, `Alt+ok`, `v`, `o`, kalıcılık, `terminal_safe` (C0, C1, bidi), senkron durumlarının işçiden gelmesi.

**Elle:** Windows Terminal + Fedora (GNOME Terminal) — Türkçe karakter girişi/görünümü, `edit`/`micro`/`nano`/`vim`
ile askıya alma/geri dönme, pencere yeniden boyutlandırma.

## Kapsam dışı
LSP, eklenti, çoklu imleç, makro, takas dosyası; fare desteği (FTXUI varsayılanı dışında), PDF dışa aktarma (CLI/GUI'de), tema seçimi.

## Değişiklik 2026-11-02

Ölçüm (T71, Release, 1 KiB'lik 2.000 not, `folder copy perf`): klasör kopyası not başına yazmayla 28,7 sn (her yazma görüntüyü yeniden kurar, UI iş parçacığında donar); `NoteStore::copy_folder` toplu işlemiyle 105 ms. Aynı not başına maliyet çoklu not kopyası ve `d` klasöründe duruyor.

Ağaç, `t`, satır görünümü, ikonlar (T74); kararlar kullanıcının.

- **ADDED** "Ağaç": `Space` klasörü yerinde açar, `open_folders` kalıcı. Klasör ayrıntısı `i`'ye taşındı; `Space` notta ayrıntıdır.
- **ADDED** `m`: klasör girdisiyle taşı, `InputKind::Folder`. Kısayol adları İngilizce sözcüklerden gelir (yank, paste, move).
- **MODIFIED** Satır: en soldaki `>` kalktı, seçim satır vurgusudur; `▸` `▾` yalnız klasörde; notta ok ve `▸` `▾` yok. Notta oka tıklama kalktı.
- **MODIFIED** `icons` `nerd | off`, varsayılan `nerd`; `auto` `nerd` okunur, `note config set icons auto` kabul edilir. Düz Unicode semboller (`▰`, `≡`) kalktı.
- **MODIFIED** Renkler: md camgöbeği, c/cpp eflatun, py yeşil, json sarı, klasör mavi; 16 ANSI.

Çoklu seçim, yank, `g` öneki (T71); kararlar kullanıcının.

- **ADDED** Bölüm "Çoklu seçim ve yank"; `x` `X` `Ctrl+A` `y` `p`; çöpte `x` ile `u` `D`.
- **MODIFIED** Listede ve çöpte `g` tek başına başa gitmez, önektir: `gg` `gh` `gt`. REVIEW'da `g` `G` aynı.

## Değişiklik 2026-11-01

`a` ve uzantı kuralı (T66); kararlar kullanıcının.

- **MODIFIED** `a` alt satırı kullanmaz, metni içerik saymaz: listenin başında ad satırı, `Enter` boş not + EDIT. Eski tek satırlık hızlı not `note "metin"`te kalır. `tui.quick_add_*` emekli.
- **ADDED** Bölüm "Uzantı kuralı". Sessiz değişim şuralardaydı: `n` çok noktalı adı (`a.tar.gz` -> `a-tar.gz`), `r` ve GUI formu (`deneme.cpp` -> `deneme.cpp.md`), `note -n` (`deneme.cpp` -> `deneme.cpp.md`).

T63 sonrası: `KEY=değer` değeri her tipte öneriden çıktı, `sh`/`json` ve benzerleri yalnız anahtar verir; küçülen not REVIEW sürüklemesinde `out_of_range` atmaz, başka metinden parça kopyalamaz; hunspell yazması durma isteğini görür.

G4 birleşimi: Liste `local_seq`/`created_local_seq` değerlerini düşürüyordu (`v` senkron sütunu ve `sync:` her notu `synced` sayardı); çöp satırı `created` ve `origin` taşımıyordu (ayrıntıda "bilinmiyor").

Bulunurluk ve hız (T64); kararlar kullanıcının.

- **ADDED** Üst satırda `çöp: N`, çöp görünümünde `ÇÖP KUTUSU` başlığı, sağda salt okunur içerik, `E` boşalt; satır işareti `▸` `▾`; `F2` yol girdisi; `Space` `i` `I`; satır numarası ve toplamlar; REVIEW'da `Ctrl+U` `Ctrl+D` yarım sayfa, `J` `K` 5 satır, `↑↓` tuş tekrarında hızlanır (1'den sayfanın üçte birine, duraklamada sıfırlanır).
- **MODIFIED** Ayarlar `F2`den `,` ve `F9`a taşındı. `PgUp` `PgDn` Listede listeyi sayfalar, önizlemeyi yalnız REVIEW odağında (çöp görünümü hariç: orada odak listede kalır) kaydırır. Sağ sütun varsayılanı boyut + tarih.
- **MODIFIED** Hız: hızlı gezinmede önizleme okunmaz, yalnız son seçim 60 ms sonra okunur; tek hareket beklemez. Olay döngüsü girdi gelince uykudan uyanır ve kuyruk boşalana dek uyumaz (FTXUI 7.0.3 tek okumada 128 bayt alır: kare başına en çok 42 ok tuşu).

fzf hızında gezinme ve ncdu tarzı tek tuş kısayolları (T51). Harfler aynı anda süzüp eylem yapamadığı için açılış
odağı Liste oldu; karar ve gerekçe kullanıcının.

- **ADDED** Üç odak (`Focus`: Liste, Arama, Önizleme) ve odak görünümü (kalın çerçeve, imleç yalnız Aramada).
- **ADDED** Liste harfleri `j k g G l / n a d r s ? q`; Önizleme `j k g G h`, `←` `→` panel geçişi.
- **ADDED** Hızlı ekleme (`a`): tek satırlık giriş, `note "metin"` kurallarıyla kayıt; kabul 8.
- **ADDED** Kabul 7-10.
- **MODIFIED** Yazı artık yalnız Arama odağında süzer; açılışta önce `/` gerekir. Kabul 2.
- **MODIFIED** `Esc`: Aramada süzgeç kalarak Listeye döner; temizleme/çıkış yalnız Listede.
- **MODIFIED** `Enter`: nvALT yeni not yalnız Aramada; Listede ve Önizlemede boş listede etkisiz.
- **MODIFIED** Önizleme odağında `Ctrl-O/R/D/S/P/N` ve `Tab` çalışmaz (kullanıcı kararı: liste komutları içerik panelinde çalışmaz). Yalnız `Ctrl-C`, `F1`, `F2` kalır. Neden: panelin geleceği (gömülü editör olabilir) açık.
- **MODIFIED** Yerleşim: dış kenarlık ve ayraçlar yerine panel başına çerçeve; alt çubuk odağa göre `tui.key_hints_{list,search,preview}`. `tui.key_hints` emekli.

Gömülü editör (T52); kararlar kullanıcının: önizleme `cat`, düzenleme `micro`, ikisi de sağ panelde.

- **ADDED** `Focus::Editor` ve bölüm "Gömülü editör"; micro benzeri tuşlar; kabul 11-15.
- **ADDED** `n` ad istemi ve kaydedilmemiş taslak: ilk kayda dek dosya yok, ilk kayıtta `detect`.
- **ADDED** Önizlemede ve Editörde not içi arama (`search::fold`), `Focus::PreviewFind`.
- **ADDED** Panel genişliği, yakınlaştırma ve fare (`PaneLayout`, `route_mouse`). `ResizableSplitLeft` yerine saf model: panel başına çerçeve ve terminalsiz testler korunur, aynı sınırlar tuş ve fareyi yönetir.
- **ADDED** SIGTERM/SIGHUP/SIGINT işleyicisi: döngü normal biter, kirli metin kaydedilir, çıkış 128+N.
- **MODIFIED** `→` Önizleme açar; `Enter`/`e` sağ panelde düzenler; harici editör yalnız `E`. `Ctrl-O` = `n`, tam ekran açmaz.
- **MODIFIED** Arama girdisi alta indi; üst satırda yalnız mod, sayaç, senkron. `Enter` seçimi tutup Listeye döner. Yer tutucu `tui.query_placeholder` emekli.
- **MODIFIED** Önizleme uzun satırı sarar; kontrol baytları editörle aynı yoldan temizlenir.
- **MODIFIED** `app.ForceHandleCtrlC(false)` ve `ForceHandleCtrlZ(false)`: FTXUI varsayılanı işlenen tuşta da sinyal yükseltiyordu. Listede `Ctrl-C` artık sinyalle ölmeden çıkar, son senkron bekler; `Ctrl-Z` editör dışında askıya almaya devam eder.
- **MODIFIED** Yeniden tarama ve senkron durumu `Event::Custom` ile çizimi tetikler: FTXUI kapanış görevinden sonra kareyi geçersiz saymaz, değişiklik ilk tuşa dek görünmüyordu.
- **REMOVED** Yeni notta geçici dosya + harici editör + kaydet diyaloğu akışı; `r` diyaloğu aynen kalır.

Yeni not, kip çerçevesi, sözdizimi vurgusu, çıkış tuşları ve editör görünümü (T58); kararlar kullanıcının.

- **ADDED** `n`/`Ctrl-O` tam terminal boş editör; ad ilk `Ctrl+S`'te, `detect` önerisiyle (anahtar/değer tiplerinde değer dosya adına girmez). Kabul 3.
- **ADDED** Kip başına çerçeve rengi ve etiketi: `LIST`, `REVIEW`, `EDIT`, `*`. Salt-okunur not `REVIEW`dur: `EDIT` yazıp tuşların etkisiz kalması kafa karıştırırdı. Kabul 16.
- **ADDED** Bölüm "Sözdizimi vurgusu". Ölçüm (Release, 120×40, tuş + yerleşim + çizim, `editor latency`): java 1 MiB satırlı medyan 0,16 ms, en kötü 1,86 ms; 1 MiB tek satır (64 KiB üstü, düz) medyan 3,2 ms, en kötü 5,1 ms; eşik altı 1,88 MiB satırlı (sonda ilk kare tüm durum taramasını öder) en kötü 3,40 ms; 10 MiB (düz metne düşer) satırlı en kötü 0,40 ms, tek satır medyan 22-31,5 ms, en kötü 34,3 ms.
- **ADDED** Editör görünümü: numara sütunu, gerçek imleç, geçerli satır, `~`, durum çubuğu; tıklamayla imleç. Kabul 17.
- **MODIFIED** Editörden çıkış `Ctrl+Q`; `Esc` yalnız girdiyi kapatır ya da seçimi kaldırır. `q` Önizlemeden de çıkar; Listede `Esc` çıkmaz. `Ctrl-C` Editör dışında acil çıkış.
- **MODIFIED** Editörde imleç ters renkli hücre değil gerçek terminal imlecidir.
- **MODIFIED** `key_hints_editor` tam tuş adlarıyla (`Ctrl+S`, `Ctrl+Q`, `Ctrl+F`, `Ctrl+Z`, `F1`); `new_note_label` ad girdisi etiketi oldu.
- **REMOVED** Önce ad soran `n` akışı. `a` aynen kalır.

Not adında sır ve Windows test derlemesi (inceleme bulguları).

- **MODIFIED** Ad önerisi `core::note_name_source`tan gelir: ilk satır `[export ]IDENT=değer` ya da `IDENT: tek-sözcük` ise yalnız anahtar, tipten bağımsız (`detect` tek satırı md sayar); sır taşıyan tiplerde yorum, bölüm ve liste satırları atlanır, anahtar yoksa zaman damgası. `Toplantı: bütçe`, `Not: süt al` ve `https://x` düzyazıdır, aynen kalır. `n`, taslak çıkışı, `a`, kaydet diyaloğu ve CLI aynı kuraldan geçer.

Sistem panosu, fare seçimi, tamamlama, yazım denetimi, genel arama, çöp, satır ayrıntısı, sıralama (T61, T60, T55b); kararlar kullanıcının.

- **ADDED** Bölüm "Pano", bölüm "Tamamlama ve yazım denetimi". Kabul 18-20.
- **ADDED** `f`, `T`, `c`, `v`, `o`, `u`, `D`, `Alt+ok`; `/` argüman alır. `Focus::SortMenu`, `Mode::Global`. Yan etkiler `ListActions`te: `main` CCN 72'den 69'a indi.
- **ADDED** `display_text`: `terminal_safe` yol ve kesiti çizimden hemen önce arındırır (C0, DEL, C1, bidi, geçersiz UTF-8); vurgu ofsetleri ham metne göredir, parça parça arındırılır.
- **ADDED** `SyncStatusWorker`: `fill_sync_status` işçide, sonuç `Post` ile; yeniden taramada durumlar korunur, tarama sonrası tekrar istenir.
- **MODIFIED** `d` ve `Ctrl-D` artık çöpe atar, onaysız (geri dönüşlüdür). Eski onaylı kalıcı silme `D`dir. Arama odağındaki `Ctrl-D` de çöpe atar.
- **MODIFIED** Argümanlı sorguda eşleşme yokken `Enter` taslak açmaz: argüman metin değildir.
- **MODIFIED** Arka plan çizimi `Event::Custom` yerine `redraw_event`: Custom, Ctrl+Space'in NUL baytıyla aynıdır.
- **MODIFIED** Önizleme üretim numarası okumadan sonra alınır: argüman sırası belirsizdi, sayı bir kare geride kalıp yeni seçimi silerdi.
- **MODIFIED** `tail_code_points(…, 1)` sınır dışına okuyordu; uzun hata girdiye tek hücre bıraktığında tetiklenir, `…` döner.
- **REMOVED** Kapsam dışından: sistem panosu, otomatik tamamlama, yazım denetimi.

T55c: `ignored` notlar uzak dal alınmadan da `sync_off`; log satırlarında kontrol baytı `\\xNN`; `committed` ve `pulled` not değişikliğini sayar (`.trash/` hariç; rename algılama seçilmedi, benzerlik eşiğine bağlıdır).

Ölçüm (T60, Release, 120×40, tuş + yerleşim + çizim, öneri, 200.000 sözcüklük tablo, yazım önbelleği): 1 MiB satırlı medyan 0,79 ms, en kötü 1,53 ms; 1 MiB tek satır medyan 3,5 ms, en kötü 3,6 ms; önekin her yerde geçtiği 1 MiB `aa aa` en kötü 5,7 ms; liste süzme en kötü 2,1 ms. 10 MiB satırlı medyan 0,87 ms, en kötü 0,93 ms; tek satır medyan 31,4 ms, en kötü 34,7 ms; `aa aa` en kötü 37,4 ms; liste süzme en kötü 3,0 ms.
