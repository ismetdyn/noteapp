# Spec: tui

> Modül: `tui` (`notes` çalıştırılabiri, FTXUI) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

`notes` yazınca açılan, klavyeyle fzf hızında gezinilen iki panelli not uygulaması. Önizleme `cat` gibi okunur, düzenleme `micro` gibi hissettirir, ikisi de sağ panelde. Düzenleme sağ panelde, yeni not (`n`) tüm terminali alan gömülü editörde olur; harici editör yalnız açıkça `E` ile.

## Yerleşim

```
notlar/work                          güncelleme↓ · [ad] · 42/5000 · ✓ 14:32
╭ LİSTE ─────────────────────╮╭──────────────────────────────────────╮
│ 1 ▾ work/                  ││# Toplantı notları                    │
│   1   plan.md              ││- bütçe onayı                         │
│ 2 ▸ personal/              ││...                                   │
│ 3   meeting-notes.md       ││                                      │
╰────────────────────────────╯╰──────────────────────────────────────╯
 LİSTE  Klasör: 3 öğe · Toplam 42 not · sayfa 1/1
: komut  T çöp  Space aç  o sırala  v sütun  f ara  Enter düzenle      F1 yardım
```
- Sol: geçerli klasörün çocukları (bkz. "Klasör görünümü"); aramada ve çöpte tam yollu düz liste, eşleşen karakterler vurgulu. Sağ: Liste ve Aramada seçili notun önizlemesi; seçim değişince aynı karede güncellenir. Önizlemede uzun satır kırpılmaz, panele sarılır (`cat`). Editör odağında aynı panel gömülü editördür; editörden seçim değişmez.
- Üst: solda klasör yolu (adlar kalın, `/` soluk); sağda soluk ve ` · ` ayraçlı sıra, mod (`[ad]` / `[içerik]`), süzgeçte eşleşme/toplam, klasör görünümünde ve çöpte yalnız not sayısı (`6 not`; satır klasör de sayar, oran yanıltır); en sonda senkron durumu kendi renginde: `kapalı` soluk, `✓ saat` yeşil, `⟳` mavi, `⚠` kırmızı (renk tek sinyal değildir, ikon da söyler). Satıra sığmayan senkron metni sonda `…` ile kısalır; sıra, mod ve sayaç tam kalır.
- Alt: iki satır. Üstteki **kip hapıyla** açılır: ` LİSTE ` / ` İNCELEME ` / ` DÜZENLE ` (editör açıkken `DÜZENLE`, yoksa odaktaki panelin kipi) kip renginde zemin, siyah kalın metin; renksiz terminalde (`NO_COLOR`) ters ve kalın. Hapın yanında toplamlar, Editörde durum çubuğu.
- En alttaki satır odağa göre kısayol çubuğu (i18n) ya da tek satırlık girdi (arama, not içi arama, ad istemi, yol girdisi). Tuş kalın ve kip renginde, açıklama varsayılan renkte; ipucu `tuş açıklama` biçimindedir, tuş boşluk taşımaz (`</>`, `PgUp/PgDn`). **`F1 yardım` sağ kenara sabittir**, en küçük pencerede de görünür (Liste, REVIEW, çöp, Editör); kalan ipuçları katalog sırasıyla soldan dolar, sığmayan ipucu ve ardındakiler bütün olarak düşer. Liste, REVIEW ve çöpte ilk ipucu `: komut`tur. Listede `:`, `t`, `Space`, `o`, `V`, `s` 61 sütundan (İngilizcede 60'tan) görünür; 60 sütunda Türkçe `f ara` F1'e yer bırakır. Seyrek olanlar F1'dedir. Listede kalan süzgeç (`> sorgu`) ve Önizlemede eşleşme sayacı (`3/12`) ipuçlarının başındadır; uzun süzgecin sonu kısalır, ilk dört ipucunu ve F1'i düşürmez. Editörde ileti, gizli karakter adı ya da tanı sığmazsa sonu `…` ile kısalır, F1'i itmez. En küçük boyut 60×15; altında "pencere çok küçük" mesajı.
- **Odak görünümü:** iki panel de yuvarlak çerçevelidir, içerik sol kenarın içinde bir boş sütundan sonra başlar (fare ve editör imleci aynı payı kullanır); odaklı panel kipe göre renkli çerçeve ve kalın etiket alır, diğerinin çerçevesi soluktur (`NO_COLOR`'da da kalın ve soluk kalır): `LIST` camgöbeği, `REVIEW` yeşil (Önizleme ve salt-okunur not), `EDIT` eflatun, kaydedilmemişse `EDIT *`. Renk tek sinyal değildir. Girdide imleç `▏`, Editörde gerçek terminal imleci. Seçim yalnız satır vurgusudur: liste odağında ters renk, sağ panel odaklıyken soluk arka plan. Satır: seçim işareti hücresi (`*`), girinti, sağa yaslı soluk numara (genişlik en büyük numaradan), klasörde `▸` (kapalı) ya da `▾` (açık), not satırında aynı iki hücre boş, ikon, ad, sağ sütun. Notun açık ayrıntısı yalnız girintili bloktur, işareti yoktur. Oka tıklamak klasörü yerinde açar/kapatır; not satırına tıklamak yalnız seçer.
- **Toplamlar:** kısayol çubuğunun üstünde, kip hapından sonra klasör görünümünde `Klasör: 12 öğe · Toplam 248 not · sayfa 2/7`, süzgeçte `Toplam 248 not · 12 eşleşme · sayfa 2/7` (eşleşme yalnız süzgeçte; sayfa = seçili satır / panel yüksekliği, kaydırma sanaldır). Editörde aynı satırı durum çubuğu kullanır.
- **Sağ sütun (`v`):** varsayılan boyut + tarih (`2.0 KiB  12:03`, bugün değilse yalnız tarih). Dar panelde önce tarih, sonra boyut düşer, değer yarım kesilmez; yol en az 16 hücre tutar. `created` ve `origin` boşken "bilinmiyor".
- **Üst satır:** çöp doluysa en solda `çöp: 3`, tıklayınca çöp görünümü açılır; boşsa yok. Yalnız metin: simge karakterinin genişliği terminale göre değişir, satır kayardı. Çöp görünümünde aynı yerde `ÇÖP KUTUSU` başlığı.
- **Boyut:** İlk açılışta sol liste pencerenin `tuning.tui_list_share` (0,33) kadarıdır, sağ panel bariz geniştir. Liste ve Önizlemede `<` `>` sol paneli 5 sütun değiştirir (sol en az 16, sağ en az 24 sütun; 60 sütunda da geçerli), `z` odaklı paneli tam ekran yapar ve geri alır (F11 değil: Konsole'un). Fareyle bölücü sürüklenir. Seçilen genişlik `<state>/tui-state.toml`da `list_share` olarak kalır (kullanıcı kararı 2026-11-05): `<` `>` her basışta, sürükleme bırakılınca yazılır; sonraki açılışta `tuning.tui_list_share`'i geçer. Sütun değil pencere oranıdır (seçilen sütunun ortası, `(sütun + 0,5) / pencere`): aynı pencerede aynı sütuna döner, terminal büyüyüp küçülünce liste orantılı kalır. Okurken yukarıdaki sınırlara çekilir (60×15'te de iki panel sığar); 0..1 dışı ya da sayı olmayan değer uyarıyla yok sayılır, config'in oranı geçer. Anahtar silinince config'in oranı döner. Yakınlaştırma oturumluktur. Yakınlaştırmada odak değişirse gösterilen panel de değişir.
- **Fare:** bölücü sürükleme, tekerlek işaretçinin altındaki paneli kaydırır (liste seçimi / önizleme / editörde imleç), satıra tıklama seçer, panele tıklama odaklar; Editörde tıklama imleci o hücreye koyar (numara sütunu metin değildir, üstüne tıklama satır başıdır). Listeye tıklamak editörü kapatmaz (GUI ve `Ctrl+K` gibi): bekleyen düzenleme notuna yazılır, sekme açık kalır, odak listeye geçer; kayıt olmazsa (çakışma sorusu, hata) tıklama yok sayılır. Kapatmak `Ctrl+Q` / `Ctrl+W`'dir. Açık soru, girdi ya da modal fareyi yok sayar. REVIEW'da sürükleme metin seçer (bkz. "Pano"); fare yakalandığı için terminalin kendi seçimi `Shift`+sürükle ile yapılır (yardımda yazar).

## Tuşlar

Dört odak vardır: **Liste** (açılış), **Arama**, **Önizleme**, **Editör**. Harfler aynı anda hem süzüp hem eylem yapamaz; harf kısayolları Liste ve Önizlemede, yazı girdilerde ve Editörde çalışır. Diyalog, ayarlar, yardım ya da tek satırlık girdi açıkken tüm tuşlar onlara gider. Harf eşleşmesi birebirdir: `ı` ve bilinmeyen harfler yok sayılır.

**Liste**

| Tuş | Eylem |
|---|---|
| `↑` `k`, `↓` `j` | Listede gezin, sağ panel canlı izler |
| `gg` `G` | İlk / son satır. `g` öneki sıradaki tuşu bekler, alt satır ne beklediğini söyler (süre yok): `gg` baş, `gh` kök klasör, `gt` çöp; başka tuş vazgeçer ve başka iş yapmaz |
| `v` `X` `Ctrl+A` | Satırı seç ve aşağı in, seçimi temizle, görünen satırları seç (bkz. "Çoklu seçim ve yank") |
| `y` `p` | Seçimi ya da satırı kopya için işaretle, geçerli klasöre yapıştır |
| `PgUp` `PgDn` | Önizlemeyi kaydır |
| `→` `l` `S` | Notta önizleme odağı (`S` göster, CLI `-S`), klasörde içeri girer |
| `←` `h` | Üst klasör, seçim çıkılan klasörde kalır; kökte etkisiz. Arama sonucunda notun klasörüne gider |
| `Enter` `e` | Seçili notu sağ panelde düzenle, klasörde içeri girer; liste boşsa etkisiz |
| `E` | Harici editörde aç (`EDITOR`); terminali ona bırakır |
| `F2` | Altta tek satırlık yol girdisi: klasör görünümünde geçerli klasöre göre ad ön dolu (düz listede tam yol), `/` içeren yol taşır, imleç adın sonunda; klasörde `rename_folder`; `Enter` `NoteStore::move` (hedef doluysa hata girdide kalır, ezme yok), `Esc` vazgeçer. Önizlemede de çalışır |
| `PgUp` `PgDn` | Listede sayfa sayfa gezer (önizleme yalnız kendi odağında kayar); `gg` `G` baş ve son |
| `,` | Ayarlar (Önizlemede de); `F9` her odakta |
| `/` | Arama girdisi (altta) |
| `a` `Ctrl-O` | Yeni not: tüm terminali alan boş editör (CLI `-a` düz ekleme) |
| `n` | Listenin başında odaklı ad satırı (CLI `-n` adla ekleme), ad geçerli klasöre göredir; `/` ile biten ad klasör oluşturur, Listede kalır, klasör seçilir, editör açılmaz (bkz. "Yeni not", "Klasör görünümü"); `Enter` boş notu oluşturur ve sağ panelde EDIT'te açar; `Esc` ve boş girdi hiçbir şey yazmaz; geçersiz ya da dolu ad satırın altında kırmızı görünür, satır metniyle açık kalır |
| `s` (`f` eski harf, palette görünmez) | Genel arama girdisi (altta, CLI `-s`): ad ve içerik birlikte (`search_all`), içerik eşleşmesinin en iyi satırı yolun altında; `Enter` Önizlemede eşleşmeye kayar; Listede `Esc` kapatır |
| `d` `Ctrl-D` | Çöpe at, onaysız; seçim komşu satıra geçer. `u` bu oturumda son atılanı geri alır. Klasörde bkz. "Klasör görünümü" |
| `D` | Kalıcı sil (onay, varsayılan "Vazgeç") |
| `t` | Çöp görünümü (haritada `t` etiketlerdir, çöp `gt`): `↑↓` gezin, `u` seçileni geri getir, `D` kalıcı sil (onay), `E` tümünü boşalt (onay, varsayılan "Vazgeç"), `q` `Esc` `t` geri. Liste komutları çalışmaz. Sağ panel çöpteki notu salt okunur gösterir (`PgUp` `PgDn` ve tekerlek kaydırır; odak listede kalır) |
| `c` | Klasörde yolunu kopyalar; notta özgün baytlarını sistem panosuna kopyalar; ikili ya da UTF-8 olmayan notta nedenini söyler, kopyalamaz |
| `Space` | Klasörü yerinde aç/kapat (bkz. "Ağaç"); notta ayrıntıyı aç/kapat |
| `Alt+→` `Alt+←` `I` | Seçili satırın ayrıntısı, klasörde de; `I` tüm satırları. Çöpte `i` de; Listede `i` içe almadır |
| `i` | İçe al: altta yol girdisi (palet `import`, CLI `-i`) |
| `x` | Seçili notu PDF olarak dışa aktar: altta yol girdisi (palet `export.pdf`, Önizlemede `P`, CLI `-x`) |
| `T` | Etikete göre ara (palet `search.tag`, CLI `-T`) |
| `m` | Seçimi ya da satırı klasöre taşı: altta tek satırlık girdi, geçerli klasörle dolu (bkz. "Ağaç") |
| `L` | Seçili notu başka nota ilişkilendir (`RELATED_TO`): altta tek satırlık girdi (bkz. "Bağlantı bölümü"). Önizlemede de çalışır |
| `Alt+→` `Alt+←` | Satırı aç / kapat: boyut, güncelleme, oluşturma, senkron, kaynak; `created` ve `origin` boşken "bilinmiyor". Birden çok satır açık kalır, gezinme not satırlarını yürür |
| `V` | Sağ sütun döngüsü: boyut + tarih, boyut, güncelleme, oluşturma, senkron, kaynak, yok. `tui-state.toml`da `version` yoksa eski `column = "none"` yeni varsayılana taşınır |
| `o` | Sıralama menüsü (alt satır): `n` ad, `u` güncelleme, `c` oluşturma, `s` boyut, `r` ters, `Esc` vazgeç; etkin sıra üst satırda |
| `r` | Ad/klasör/tip diyaloğu; klasörde `F2` |
| `Ctrl-S` | Şimdi senkronla (Önizlemede de) |
| `<` `>` `z` | Panel genişliği, yakınlaştırma |
| `:` | Komut paleti (bkz. "Komut paleti"); REVIEW ve çöpte de |
| `?` | Yardım |
| `q` | Çık |
| `Tab` | Ad ↔ içerik modu (Aramada `Ctrl-T`) |
| `Esc` | Bekleyen yank varsa ondan vazgeç; süzgeç doluysa temizle; boşsa etkisiz, çıkmaz |

Sıra ve sağ sütun `<state>/tui-state.toml`'da kalır; bozuk değer varsayılana döner (`updated` azalan, sütun yok).

`/` ve `s` her zaman kökten, klasörler arası düz sonuç verir: tam yol, sonuçlar boyunca numara. `/` metin yazılana dek klasör görünümünü bırakır. `/` ve `s` sorgu argümanlarını alır (`SPEC-search`): `sort:-size` `type:md` `in:work/` `size:>1mb` `updated:7d` `sync:modified` `tag:proje` `view:iş`. Bozuk değer metin sayılmaz: girdide yeriyle `format_error` görünür, liste son geçerli haliyle kalır, taslak açılmaz. Argümanlı sorguda eşleşme yokken `Enter` taslak açmaz. `sort:` yoksa metinli sorgu fuzzy sırasındadır, boş sorgu `o` sırasındadır.

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

**Girdi satırı (`InputLine`):** `s`, `/`, `F2`, ad istemi, editörde bul, `n` satırı ve komut paleti aynı saf bileşeni kullanır (`tui/input_line`, FTXUI'siz; çizim `tui/input_view`). REVIEW'daki `/` ve kaydet diyaloğu dışarıdadır.

| Özellik | Davranış |
|---|---|
| Gölge öneri (soluk) | İmleç metnin sonundayken. `s` `/`: önce geçmiş (tüm satır), sonra son sözcükle başlayan not adları. `n` `F2` ad istemi: klasörler, sonra adlar. Editörde bul: nottaki sözcükler. `→` alır |
| `Tab` | Adaylar: `klasör/`, not adları, `.cpp` gibi tipler (yolda yalnız uzantıyı değiştirir); `s` `/`'de ayrıca `sort:name\|size\|updated\|created`, `type:<tipler>`, `in:<klasörler>`, `size:`, `updated:`, `created:`, `origin:windows\|linux\|macos\|android\|unknown`, `sync:`, `tag:<notların etiketleri>`, `view:<kayıtlı görünümler>`. Tek aday doğrudan yazılır, çoksa fzf gibi liste: yazı süzer, `↑↓` gezer, `Enter` ekler, `Esc` kapatır. Boşluklu aday `s` `/`'de tırnaklanır |
| Renk (`s` `/`) | Geçerli argümanın anahtarı camgöbeği, değeri yeşil; bozuk değer `parse_query`'nin yerini bildirdiği aralıkta anında kırmızı; düz metin normal. Aralıklar bayt ofsetidir, UTF-8 imleci bölmez |
| Geçmiş (`s` `/`) | `Enter`le kullanılan arama `<state>/tui-state.toml`da `search_history`e girer: en yeni başta, yinelenmez, en çok `limits.tui.search_history` (200), kayıt başına en çok `limits.tui.search_entry_bytes` (1 KiB), senkrona girmez. Bozuk sorgu ve taslak açan eşleşmesiz sorgu girmez. `KEY=değer`, `anahtar: değer` (argüman anahtarı hariç; `core` sır kuralı) ve 20 bayt üstü harf-rakam jetonu taşıyan ya da sözcüğü bir not adıyla birebir aynı olan sorgu yazılmaz |

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
| `b` | Bağlantı bölümüne odak (bölüm yoksa etkisiz) |
| `L` | Nota ilişkilendir (Listedeki gibi) |
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
| `Ctrl+Z` `Ctrl+Y` | Geri al / yinele; yazım kelime kelime birleşir, geçmiş `limits.tui.undo_bytes` (64 MiB; 0 = sınırsız) ile sınırlı (en eski adım düşer) |
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

**Çizim:** terminale yalnız değişen hücre yazılır (`tui/frame_diff`, synchronized update ile sarılı); FTXUI'nin çıktı kancası `cmake/patches/` yamasındadır. Kuyruktaki girdi tek karede işlenir. `Ctrl+L` her odakta ekranı silip baştan çizer; harici editörden dönüş, askıdan dönüş ve boyut değişimi de öyle. Değişiklik yoksa çizim yoktur: otomatik kayıt, dil sunucusu gönderimi ve önizleme ertelemesi beklerken kare kurulmaz; en yakın süre dolunca tek yeniden çizim gönderilir (`TuiSession::wake_due`); yerel gece yarısı da bir uyanma anıdır (listedeki "bugün" saati tarihe döner). Kare isteyen yalnız haritanın yerleşim animasyonudur. Tanı: `NOTEAPP_TUI_DRAW_LOG=<dosya>` her çizimde dosyaya bir satır (terminale yazılan bayt) ekler; `tui_pty` testleri boşta çizim olmadığını bununla sayar.

**Akış denetimi:** `Ctrl+Q` XON, `Ctrl+S` XOFF'tur. FTXUI 7.0.3 oturumda `IXON`'u kapatır ve çıkışta eski termios'u geri yükler (`app.cpp`); e2e başlangıçta `IXON` açık pty'de iki tuşun ulaştığını, oturumda `-ixon`, normal çıkış ve SIGTERM sonrası `ixon` olduğunu doğrular. Bu yüzden ek terminal kodu yoktur.

İçerik modunda liste not başına eşleşme sayısını gösterir; önizleme ilk eşleşmeye kayar ve eşleşmeleri vurgular.

## Komut paleti

`:` Liste, REVIEW ve çöpte küçük bir pencere açar; tuşunu bilmeden her işi adıyla çalıştırır. Odak değişmez: kip etiketi, yakınlaştırma ve komutun çalışacağı görünüm aynı kalır. `:` EDIT'te, girdilerde ve sıralama menüsünde metindir ya da yok sayılır; bağlantı bölümü odağında açılmaz (kendi `↑↓` `L` anlamları vardır, `Esc` REVIEW'a döner).

- **Tek tablo:** `TuiState::commands()` (`src/tui/commands.cpp`). Liste, REVIEW ve çöp tuşları `handle_focused`ın başında buradan çözülür, palet aynı satırın `run`'ını çağırır: tuş ve palet aynı işi yapar. Tabloda kalmayan yalnız takma tuşlardır (`↑` `↓`, `Enter`, `Ctrl-O` gibi `handle_shared` kısayolları, `g` öneki, `Esc`); aynı metotları çağırırlar. Aynı `id` görünüm başına birden çok satır olabilir (`top`: Listede `gg`, REVIEW'da `g`); palet o görünümde bağlı olanı, yoksa ilkini tek satır gösterir.
- **Satır:** `Taşı … move  m`: solda `command.*` etiketi, sağ kenarda soluk İngilizce kararlı `id` ve tuş (takma harf yanında: `gt T`, `F1 ?`); GUI paletiyle aynı bilgi sırası. Uzun etiket kısalır, kimlik ve tuş görünür kalır. Etiket ve kimlik ortak kayıttan (`app::kCommandRegistry`, `src/app/commands.hpp`; kayıtta olmayan kimlik derlenmez). Tuşu olmayanlar (`column.*`, `tree.toggle_all`, `icons.toggle`, `search.tag`) yalnız paletten çalışır. Başlıkta hedef: seçili satırın yolu (`terminal_safe`, uzunsa sonu) ya da `seçili: 3`.
- **Süzme:** `InputLine` + `app::rank_palette` (`search::fuzzy_match`; GUI paleti de bunu kullanacak), `id` ve çevrilmiş etiket üstünde, iyisi geçerli. Sıra: çalışanlar önce, puan, son kullanılan, tablo sırası. Boş süzgeçte puan yoktur: son kullanılanlar baştadır; süzerken eşit puanda öne geçer. `recent_commands` `tui-state.toml`da kalır, en çok 20; bilinmeyen ad yer tutmaz.
- **Çalışamayan:** satır soluk, nedeni parantezde; seçiliyken ipucu satırında tamamı. `Enter` çalıştırmaz, pencere açık kalır; karar `Enter` anında yeniden hesaplanır. Nedenler: başka görünümün komutu, not ya da satır seçili değil, bağlantı yok, klasör görünümü değil, yank yok, senkron kapalı. `Yank` ve `Sync` tuşta engel değildir: tuş eski gibi nedeni alt satırda söyler.
- **Tuşlar:** yazı süzer, `↑↓` gezer, `Enter` çalıştırır, `Esc` kapatır. `F1` `F9` paleti kapatıp açar, `Ctrl-C` çıkar. Fare yok sayılır.
- **`search.tag`** (Etikete göre süz): Listede `/` ad aramasını `tag:` yazılı açar ve `Tab` listesini gösterir (notların etiketleri; tek etiket doğrudan yazılır). Süzme `tag:` argümanının kuralıdır (SPEC-search).
- **`search.view`** (Görünüm çalıştır, T81): `search.tag` gibi; `/` ad aramasını `view:` yazılı açar, `Tab` listesi kayıtlı görünümlerdir (fzf gibi süzülür, tek görünüm doğrudan yazılır, boşluklu ad tırnaklanır). Seçilen `view:AD` listeyi görünümün döndürdüğü notlara süzer (SPEC-search `view:`); bilinmeyen ya da yazan görünüm girdide kırmızı hatadır, liste son geçerli haliyle kalır. Görünüm listesi açılışta ve yeniden taramada (`graph_version`) depodan gelir; yeni pane yoktur.
- **`tree.toggle_all`:** tüm klasörler açıksa kapatır, değilse açar (`open_folders`). **`icons.toggle`:** `icons` `nerd` ↔ `off`, `config.toml`a Ayarlar ekranının kuralıyla yazar (okunamayan dosya ezilmez, hata alt satırda), anında uygulanır.
- **Eşitlik:** `F1` metni elle yazılır, tablo testle bağlıdır: `command_palette_test.cpp` her satırın tuşunu palet ve F1 çiziminde, her çalışabilir komutun tuşla aynı durumu ürettiğini denetler; e2e aynısını gerçek terminalde yapar.

## Klasör görünümü

Varsayılan liste klasör tabanlıdır (`src/tui/folder_tree.*`, saf). Ağaç `NoteStore::list` ve `folders`in bellekteki anlık görüntüsünden yalnız liste değişince kurulur (`O(not × derinlik)`); gezinme, klasöre girme ve çıkma depoya gitmez, klasör satırı önizleme okuması tetiklemez.

- **Satır:** önce klasörler, sonra notlar; `file_icons` ikonu (klasörde açık ya da kapalı ikon), ad kısa (`arşiv/`, `plan.md`), klasör adı `/` ile biter. `o` her grubun içinde uygulanır: klasörde ad, alt ağacın toplam boyutu, son güncellemesi, son oluşturması; bilinmeyen tarih azalan sırada sonda. Numara klasörle birlikte 1'den başlar, klasör başına yeniden başlar; numara genişliği ve `row_marker_column` en büyük numaradan gelir.
- **Ağaç:** `Space` klasörü yerinde açar: not ve alt klasörler altında, seviye başına iki hücre girintili (panelin en çok yarısı), her derinlikte; numara kendi klasöründe yeniden başlar. `→` `l` yine içeri girer (yazi), `←` `h` her zaman üst klasöre gider, açık satırı kapatmaz. Açık küme girip çıkınca kalır, `tui-state.toml`da `open_folders`; kalmayan klasörler yazarken düşer (taramada değil: senkron geri getirebilir). Seçim, sayfalama, toplamlar, fare satır eşlemesi, `x` ve toplu işlemler ağaç satırlarında çalışır; `p` ve `m`'nin ön değeri üst satırdaki klasördür. Liste yalnız değişimde kurulur (`O(görünür satır + açık klasörlerin çocukları)`), gezinme depo okumaz, çizim görünene orantılıdır: 20.000 satırlık açılış testi vardır.
- **`m` taşı:** seçim ya da satır için altta `Taşı:` girdisi, geçerli klasörle dolu; `Tab` yalnız klasörleri önerir (boşlar dahil, ağaçtan), çoksa fuzzy liste. `Enter` not için `NoteStore::move`, klasör için `rename_folder`; yazılan iç içe klasörler oluşur. Hiçbir şey ezilmez: hedefte ad doluysa hiçbir öğe taşınmaz, hata girdide kalır. Aynı klasör hiçbir şey yapmaz; kendi içine taşıma hata verir. `Esc` vazgeçer. Kopyadan farkı: `p` ad çakışmasında `-2` verir, `m` hata verir.
- **Üst satır:** çöp göstergesi, açık klasör ikonu, `notlar/work/sub` (adlar kalın, `/` soluk). Sığmazsa baştaki parçalar `…/` olur, bulunulan klasör görünür kalır. Sağ taraf için `kTopBarRightCells` (44) ayrılır. Parçaya tıklamak o klasöre gider, ayırıcı ve `…` tıklanmaz. Adlar `terminal_safe`ten geçer.
- **Sağ sütun:** klasörde `v` seçiminden bağımsız `12 öğe` (`yok` hariç). **Ayrıntı** (`i`): yol, öğe/not/klasör sayısı, toplam boyut, son güncelleme.
- **REVIEW:** klasör seçiliyken özet: öğe sayısı, toplam boyut, son güncelleme, alt ağacın en yeni beş notu (`FolderTree::kRecentNotes`). `→` klasörde içeri girdiği için panele fareyle gelinir.
- **Dönüş:** üst klasöre çıkışta çıkılan klasör seçili kalır. Süzgeçten dönüşte klasör ve seçim yerinde (`saved_spot_`); çöpten dönüşte klasör yerinde, seçim ilk satırda. Aramada `Esc` süzgeç kalarak Listeye döner (eski kural), ikinci `Esc` klasör görünümüne.
- **Silinen klasör:** yeniden taramada geçerli klasör gitmişse en yakın duran ataya düşülür. Kaydedilen ya da seçilen not başka klasördeyse (`select_note`, `reveal`) liste oraya gider.
- **Fare:** tıklama seçer, aynı satıra 400 ms içinde ikinci tıklama klasöre girer; `▸` `▾` okuna tıklama klasörü yerinde açar/kapatır, ikinci tıklama sayılmaz.
- **İpuçları:** klasör satırında ve boş klasörde `key_hints_folder`: `: komut  → aç  ← üst  a ekle, ad/ = klasör  F2 ad değiştir` 60 sütunda görünür; `d çöpe at` 66 sütundan sonra.

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

- **Seçim:** `v` satırı (not ya da klasör) seçer ve aşağı iner, `X` temizler, `Ctrl+A` görünen tüm satırları seçer; arama sonuçlarında tam yolla çalışır. Satırda `*` ve sarı ad; renk tek sinyal değildir. Klasör değişince kalır, alt satırda `seçili: 3`. Yeniden taramada kaybolan öğe düşer. Çöpün ayrı seçimi vardır (ayrı ad uzayı), çöpten çıkınca silinir.
- **Hedef:** `d` `D` `y` `m` (taşıma, bkz. "Ağaç") seçim doluysa onu, değilse geçerli satırı işler. Seçili klasörün içinde ayrıca seçilmiş öğe tek sayılır, ikinci kez işlenip hata vermez.
- **`d`:** onaysız çöpe; seçimde notlu klasör varsa toplam sayıyla sorar, varsayılan Vazgeç (tek klasör kuralı). Tek `u` adımın tamamını geri alır.
- **`D`:** kalıcı, toplam öğe ve not sayısıyla sorar, varsayılan Vazgeç.
- **`y` `p`:** `y` kopya için işaretler, seçim tüketilir, alt satırda `kopya: 2`. `p` geçerli klasöre yapıştırır, düz listede klasör ister; kaynak yerinde kalır. Not kopyası baytları okuyup `create_exact`, klasör kopyası `NoteStore::copy_folder` (boş alt klasörleriyle, toplu işlem). Hiçbir şey ezilmez: dolu ad `-2`, `-3` (`a.tar.gz` -> `a.tar-2.gz`; klasör `iş-2`), her biri raporlanır. Klasör kendi içine kopyalanmaz. `Esc` bekleyen yank'ten vazgeçer. `c` sistem panosudur, yank'e karışmaz. Uzantısız not kopyalanamaz (`create_exact` tip ister), hata raporlanır, kaynak yerinde kalır.
- **Kısmi hata:** kalan öğeler sürer. Sorunsuz işlem alt satırda tek ileti; hata ya da ad değişimi varsa öğe öğe küçük pencere (`2 tamam, 1 başarısız`), sıradaki tuş kapatır. `u` da aynıdır: geri gelmeyen not raporlanır, çöpte artık yoksa adımı kilitlemez.
- **Çöp:** `x` `X` `Ctrl+A` ile `u` geri getirir, `D` kalıcı siler (sayıyla, varsayılan Vazgeç).
- **Senkron:** değişiklik yapan her işlem sonda bir kez `notify_changed` çağırır.

## Yeni not

- `n` notu adıyla başlatır: listenin üstünde, liste panelinin içinde boş bir satır açılır, tuşlar oraya gider (alt satırda yalnız ipuçları). Liste satırın ve seçili notun görüneceği kadar kayar, numara sütunu hizasını korur (satırın numarası boştur, yol aynı hücrede başlar). `Enter` notu **hemen ve boş** oluşturur, sağ panel EDIT'te açılır; boş bırakılsa da kalır. Ad `klasör/ad.uzantı` olabilir, klasör görünümünde geçerli klasöre göredir; düz listede klasör yoksa `n`'deki gibi seçili notun klasörü, o da yoksa `default_folder`. Uzantı yoksa `default_type` (`detect` yok: daha içerik yok). `-2` eklenmez: dolu ad hatadır. **Süzgeç açıkken** satır süzgeçten etkilenmez; yeni not süzgeçten geçiyorsa süzgeç kalır, geçmiyorsa süzgeç temizlenir ki not görünsün (`select_note` kuralı).
- `a` ya da `Ctrl-O` önce ad sormadan, harici editör olmadan boş editörü tüm terminalde açar: liste ve üst çubuk gizlenir (panel yakınlaştırması), üst satır "Yeni not" der. **İlk kayda dek dosya yoktur.**
- İlk `Ctrl+S` altta tek satırlık ad girdisi açar, ilk satırın slug'ı ve `detect` uzantısıyla dolu gelir. `Enter` kaydeder ve düzenlemede kalır; `Esc` hiçbir şey yazmadan düzenlemeye döner. Boş taslakta `Ctrl+S` ne sorar ne dosya oluşturur. Geçersiz yol ya da yazma hatası girdide görünür, girdi açık kalır.
- Ad `klasör/ad.uzantı` olabilir; klasör yoksa yeni notun klasörü (girdinin etiketinde yazar): vurgulu klasör satırı, girilen klasör, seçili notun klasörü, o da yoksa `default_folder` (GUI ile ortak, `app::new_note_folder`); açık uzantı kazanır (bkz. "Uzantı kuralı"), uzantı yoksa `detect`. Çakışmada mevcut `-2` kuralı. Girdi boşaltılırsa zaman damgalı ad.
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
- **Karakter:** imleç grapheme sınırındadır; çok baytlı karakter ya da birleşen işaret bölünmez, CRLF tek adımdır. Genişlik FTXUI'nin ölçüsüdür. Kontrol baytları (C0, DEL, C1) ekranda `U+FFFD`, sekme sonraki 4'lü durağa boşluktur; terminale ham bayt gitmez, dosyadaki bayt değişmez. Görünmez bidi ve sıfır genişlikli karakterler (T91, Trojan Source; tek liste `core::hidden_controls`: U+061C, U+200B, U+200E/U+200F, U+202A–U+202E, U+2060–U+2064, U+2066–U+2069, metin içi U+FEFF; T91b: Hangul dolguları U+115F, U+1160, U+3164, U+FFA0 ve U+180E (boş ad sahteciliği); ZWNJ/ZWJ U+200C/U+200D bilerek dışarıda: emoji dizilerinde ve Farsça/Arapça/Hint yazılarında meşrudur, bidi sahteciliği yapmaz) kendi başına bir karakterdir (önceki harfe yapışmaz), kırmızı zeminli tek hücrelik `U+FFFD` olarak çizilir; imleç üstündeyken ileti satırı adını söyler (`Gizli karakter U+202E RIGHT-TO-LEFT OVERRIDE`). Hareket, sarma, seçim, silme ve tanı alt çizgisi onu tek hücre sayar; baytlar değişmez. REVIEW düz önizlemesi aynı yoldan çizer, baştaki BOM editördeki gibi atılır.
- **Salt okunur:** ikili (`looks_binary`), NUL içeren, geçersiz UTF-8 ve `tuning.max_note_bytes`'tan (10 MiB) büyük not. Neden durum çubuğunda görünür; kaydırma, arama, kopyalama çalışır; hiçbir yol yazmaz. Gösterilen metin arındırılmış kopyadır, büyük notta başı.
- **Boyut sınırı gerekçesi** (Release, 120×40, tuş + yerleşim + çizim): 10 MiB satırlı notta en kötü 2,8 ms; 10 MiB tek satırda medyan 32-45 ms (makine yüküne göre; yük altında tepe 70 ms'e çıkar); canlı not içi arama 10 MiB'te 34-42 ms; sınır 50 ms (SPEC.md §10/3). Daha büyüğünde tek satır bütçeyi aşacağından mevcut `max_note_bytes` sınırı olarak kaldı.
- **Otomatik kayıt:** kirli kayıtlı not son düzenlemeden `tuning.autosave_ms` sonra yazılır (GUI ile aynı); ardışık tuşlar tek yazmaya birleşir, `Ctrl+S` bekleyen süreyi düşürür. Soru açmaz, not yaratmaz: çakışma ya da silinen not durum çubuğunda (`Diskte değişti`/`Diskte silindi`) kalır, karar açık kayıtta (`Ctrl+S`, çıkış sorusu, sinyal çıkışı). Taslak, salt-okunur not ve açık arama/soru/tamamlama beklemez; kip dönünce yazılır. Kural `app::Document`/`app::Autosave` (`src/app/document.hpp`).
- **Kaydetme** yalnız `NoteStore::write` (atomik, mtime denetimli). Başarıda `notify_changed`, liste ve içerik indeksi yenilenir. Hatada tampon kalır, hata durum çubuğundadır, soru ekranından çıkılmaz.
- **Çakışma** (`ModifiedExternally`): Benimkini kopya olarak kaydet (varsayılan; `ad (copy).uzantı`, baytlar aynı, editör kopyaya geçer) / Disktekini yükle / Üstüne yaz / Vazgeç.
- **Dış değişiklik:** yeniden tarama ve senkron kirli tampona dokunmaz, durum çubuğu uyarır. Temiz tampon yenilenir, imleç aynı satır/sütunda kalır. Not silinmişse temiz tampon kapanır; kirli tamponda metin kalır, `Ctrl+S` aynı adla yeni not yazar.
- **Çıkış:** editörden `q` ve `Ctrl-C` ile çıkılamaz. SIGTERM, SIGHUP ve dışarıdan SIGINT'te kirli metin normal yoldan yazılır (taslak oluşturulur, çakışmada kopya, olmazsa geçici dosya; sonuç stderr'e), süreç 128+N ile biter. SIGKILL ve kilitlenme kapsam dışıdır (takas dosyası yok).
- **Yapıştırma:** Terminalden yapıştırma (`Ctrl+Shift+V`) bracketed paste'tir (DECSET 2004 açılışta istenir, çıkışta ve harici editörde geri alınır; FTXUI 7.0.3 sunmadığı için `cmake/patches/ftxui-7.0.3-frame-writer.patch`). Metin tek olaydır (`Key::Paste`): CRLF, tek CR ve LF birer satır sonudur, notun satır sonuyla (LF/CRLF) tek geri alma adımında girer, sekme ve girinti aynen kalır, öneri kabul edilmez, tek kare çizilir. Tek satırlık girdide (arama, ad satırı, ad istemi) sondaki satır sonları atılır, aradakiler ve kontrol baytları boşluk olur; Enter erken göndermez. Listede yapıştırma yok sayılır (harfler kısayol olmaz). `limits.tui.paste_bytes`'tan (32 MiB; 0 = sınırsız) büyük yapıştırma `Ctrl+V` gibi reddedilir, metin değişmez, neden söylenir. `ESC[201~` hiç gelmezse (bozuk terminal, kopan bağlantı) `limits.tui.paste_timeout` (1 sn; 0 = hiç) sessizlikten sonra toplanan metin yapıştırılır; ardından geç gelen baytlar tuş değil aynı yapıştırmanın devamıdır (`ESC[201~` ya da bir sessizlik daha gelince ikinci yapıştırma olarak girer), tuşlar o ikinci sessizlikten sonra yeniden çalışır. Bedeli (ödünleşim): işaret kaybolduysa bu sessizlikler dolana dek basılan tuşlar da yapıştırmanın metnidir, komut çalıştırmaz; her bayt süreyi yeniden başlatır (kısa süre yavaş akan yapıştırmayı böler, uzun süre tuşları daha çok yutar). Bitiş işaretinin ortasında `paste_timeout` kadar susma olursa işaret iki yapıştırmaya bölünür ve metin kalır: notta ESC baytı ve `[201~` görünür, tuş çalışmaz. Ayrıştırıcı sınırı işaretle birlikte sayar: işaretsiz biten yapıştırma sınırı en çok 6 bayt aşabilir, uygulama yeniden denetleyip reddeder; sınırı aşan baytlar ayrıştırıcıda da tutulmaz; bitiş işareti ve UTF-8 okumalar arasında bölünebilir, yapıştırmanın içindeki kaçış dizileri tuş değil bayttır. Bracketed paste'i desteklemeyen terminalde metin tuş olarak gelir: aynı okumadaki CR LF tek Enter'dır. `Ctrl+V` sistem panosunu okur (bkz. "Pano").

## Sekmeler

Çok belgeli editör (plan-parity T3, karar 8): her sekme kendi `EditorSession`'ını (tampon, imleç, kaydırma, geri alma, salt okunur nedeni) taşır (`tui::EditorTabs`). Kurallar GUI sekmeleriyle aynıdır (SPEC-gui "Sekmeler"); sıra, sınır, kalıcılık, komşu ve taşıma kararı ortak `app/tabs`'tadır.
- **Sekme satırı** sağ panelin üst kenarında, kip etiketinden sonra (`EDIT │ a.md │ [b.md*] │ c.md`): etkin sekme köşeli parantezde, kirli sekmede `*`; sığmayanlar etkin sekme görünür kalacak şekilde `…` olur. Kenarda olduğu için panel içeriği kaymaz. Sekme varken REVIEW'da ve Listede de görünür.
- **Açma:** `e`/`Enter`, `a`, `n`, eşleşmesiz Enter etkin sekmenin yerine açar; etkin sekme düzenlenmişse, yeni sekmede açılmışsa ya da yazılmış taslaksa yeni sekmede. Satır menüsü (sağ tık) ve palet `tab.open` seçili notu yeni sekmede açar. Başka sekmede açık not yinelenmez, o sekme öne gelir (imleci yerinde). `Ctrl+T` yeni sekmede boş taslak.
- **Geçiş:** `Alt+N` / `Alt+P` (terminal geçirirse `Ctrl+PgDn` / `Ctrl+PgUp`) Listeden, REVIEW'dan ve editörden; editöre girer. Ayrılan sekmenin bekleyen düzenlemesi yalnız kendi notuna yazılır; kayıt olmazsa (çakışma sorusu, hata) geçiş olmaz. Taslak kaydedilmeden sekmesinde kalır. Editörde soru ya da girdi açıkken sekme değişmez. `Ctrl+K` editörde sekmeyi açık bırakıp arama girdisine geçer (GUI `Ctrl+K`); bekleyen düzenleme önce yazılır.
- **Kapatma:** `Ctrl+W` mevcut akışla kapatır (temizse hemen, kirliyse Kaydet / At / Vazgeç), sonra sağdaki (yoksa soldaki) sekme editörde öne gelir. `Ctrl+Q` aynı akışla sekmeyi kapatıp Listeye döner; öteki sekmeler arka planda kalır.
- **Senkron ve eylemler:** yeniden taramada her sekme kendi notunu denetler: temiz tampon yenilenir, notu silinen (senkronda yeniden adlandırılan dahil) temiz sekme `Sekme kapandı: '<yol>' artık yok` iletisiyle kapanır, kirli tampon mevcut akışta kalır (`Ctrl+S` yeni not yazar). Liste eylemiyle taşınan not ya da klasör (`F2`, `m`, `r`) sekmelerin yolunu taşır: sonraki kayıt yeni yola gider (GUI A5b). Liste eylemi notu silerse arka plandaki sekme aynı olayda kapanır.
- **Sınır:** `limits.app.tabs` (0 = sınırsız, varsayılan; GUI ile ortak). Dolunca yeni sekme açılmaz: `En çok N sekme açılabilir` iletisi.
- **Kalıcılık:** `tui-state.toml` sonunda `active_tab` ve `[[tabs]]` (`path`, `pinned`, `kept`): GUI `gui-state.toml` ile aynı anahtarlar. Taslak yazılmaz. Açılışta artık olmayan ya da yinelenen yol atlanır; açılış Listede kalır, sekmenin içeriği ilk etkinleşmede okunur.
- **Çıkış:** her sekmenin kirli metni normal yoldan yazılır (taslak not olur; olmazsa geçici dosya, stderr'de).

## Harita

Paletteki `view.map` (plan-parity T4, karar 8) liste panelinde notları çizge olarak gösterir; `view.tree` / `view.details` listeye döner, çöp açılınca çöp listesi görünür. Oturumluktur. Model ve yerleşim GUI ile aynıdır (`app::MapModel`, `app::GraphLayout`; SPEC-gui "Harita"): aynı düğüm ve kenarlar, aynı kapsam, aynı okuma kuralı.
- **Çizim:** FTXUI braille `Canvas` (hücre 2x4 nokta). Kenar çizgidir (ilişki mor, etiket kenarı soluk), düğüm nokta (rengi tipin ikon rengi), etiket tek nokta. Gösterilen not (etkin sekmenin notu, sekme yoksa seçili not) sarı halkalı ve sarı adlıdır, Tab'la seçilen not köşeli parantezde ve ters renktedir. Komşular yeterince açıkken tüm adlar, değilse yalnız gösterilen ve seçilen not; ad kısalır, başka adın üstüne binmez, kenarda içeri kayar. Ad güvenilmezdir: `terminal_safe`. Tuval dışına düşen kenar kırpılır. İlk satır kapsam ve tuşlardır; sınır aşıldıysa `{total} düğümden {shown}`.
- **Kapsam:** varsayılan gösterilen notun çevresi, derinlik 2; `1`–`9` derinlik (en çok `limits.app.map_depth`, varsayılan 6, 0 = sınırsız; GUI adımlayıcısıyla aynı sınır, `app::MapModel::max_depth`; üstündeki rakam derinlik tuşu değildir, ipucu satırı `1-{n}` gösterir), `w` bütün çizge, `t` etiketler (GUI'deki onay kutuları ve adımlayıcı).
- **Tuşlar (Liste odağında):** oklar kaydırır, `+` `-` yakınlaştırır (panel ortası yerinde), `0` sığdırır; kaydırma ya da yakınlaştırma kendiliğinden sığdırmayı kapatır, `0` ve çizgenin değişmesi yeniden açar. `Tab` / `Shift+Tab` notlar arasında seçer, `Enter` seçili notu sekme kuralıyla açar (SPEC-tui "Sekmeler"). Diğer liste tuşları (`:`, `q`, `<` `>` `z`, `s`, `T`) çalışır; `/` `x` `X` `e` `n` `d` haritada keşif tuşlarıdır (aşağıda). `t` haritada etiketleri açıp kapatır, Listedeki çöp tuşunu gölgeler: haritadan çöpe `gt` ile gidilir (F1'de yazar).
- **İlişki kurma (MAP-R, GUI'nin Shift+sürüklemesiyle eşit; kural `app::map_relations`):** `C` (connect) Tab'la seçili nottan, yoksa gösterilen nottan kipi açar; kapsam satırı `kaynak → hedef` ve tuşları gösterir, kaynak camgöbeği, hedef köşeli parantezde, aralarında camgöbeği çizgi. `Tab` / `Shift+Tab` hedefi kaynak dışındaki düğümler (not ve etiket) arasında seçer; `Enter` kurar: not → not `r` ilişkili (`RELATED_TO`) / `l` bağlantı (kaynağın sonuna `[[tam/yol.md]]`) sorar (seçim beklerken başka harf yutulur), not ↔ etiket etiketi ekler, etiket → etiket bir şey yapmaz. `d` aradaki kenarı kaldırır (ilişki ve bağlantı birlikteyse ilişki; etiketin `#`i metinden çıkar, sözcük kalır; yalnız etiketlerden oluşan satırda etiket silinir (GUI ile aynı, `core::remove_note_tag`); bağlantı reddedilir, ileti editörde silmeyi söyler; kenar yoksa ileti). `Esc` vazgeçer; oklar, `+` `-` `0`, `w` `t` `1`–`9` kipte de çalışır. Fare: düğümden sağ sürükleme başka düğüme bırakınca `Enter` ile aynıdır (not → not `r`/`l` bekler); Shift+sol sürükleme de aynıdır ama çoğu terminal Shift+fareyi kendi seçimine ayırır, bu yüzden sağ tuş birincil yoldur. Kenara (çizginin geçtiği hücre) sağ tık o kenarı seçer, `d` kaldırır. **Adlı ilişki (F1):** `r` ad satırını açar (`L`'ninkiyle aynı: `Tab` adlar, `Ctrl+D` yönlü, kaynak → hedef); hedefle kaynak arasında (ya da sağ tıklanan) kenar ilişkiyse kapsam satırı uçlarını, yönünü (`→`/`—`) ve adını (`'uses'`, adsız `(adsız)`; `terminal_safe`, 16 hücrede kısalır) gösterir ve `n` adını değiştirir (ad satırı), `f` yönünü çevirir (yönsüzü kaynaktan hedefe yönlü yapar), `u` yönsüz yapar; hepsi kenarın kimliğiyle (`MapEdge::relation`). Aynı çiftte birden çok ilişki varsa harita tek kenarı hedefler (`app::map_edge_between`: kaldırılabilen tür önce, ilişkilerden çizgedeki ilki); çiftin her ilişkisine bağlantı bölümünün **İlişkili** satırlarından ulaşılır (`n` `f` `u` `L`). Haritaya fare basışı editörü park eder (bekleyen düzenleme notuna yazılır); işlemden sonra sekmeler yeniden okunur (temiz sekme yeni metni alır, kirli sekme ezilmez, kaydı çakışma sorar). Sonuç ve hata (eksik not, kod notu, var olan kenar, dışarıda değişmiş not) alt satırdadır.
- **Arama:** liste süzgeci (paletin `search`'ü, `s`, `T`; liste ile aynı ayrıştırıcı, `tag:` dahil) eşleşen notları renkli bırakır, gerisini soldurur; etiket eşleşen bir notunda yanar.
- **Keşif (F2, GUI ile eşit; kural `app::MapModel`, oturumluk):** hedef Tab'la seçilen not, yoksa gösterilen not. Harfler İngilizce adlardan; liste görünmediği için `x` `X` `e` `/` `n` `d` haritada gölgelenir.
  - `x` (expand) komşuları geçerli kenar süzgecinin türü ve adıyla (`app::expansion_for`) düğümden şu an görünen erimin bir adım ötesine açar (merkezde derinlik + 1), açılmışsa yalnız açılışı geri alır; `X` bir adım daha (`limits.app.map_depth`'e dek). Kapanan notun içinden kapsam geçmez. Bütün çizgede ileti: her komşu zaten görünür.
  - `e` (edges) kenar türü süzgecini döndürür: bağlantı ve ilişki → yalnız bağlantı → yalnız ilişki. `N` (name) ilişki adı satırını açar (`Tab` adları listeler: `relation_names`; `(adsız)` adsız ilişkidir; boş Enter süzgeci kaldırır), `#` etiket satırını (`Tab` etiketleri listeler; aramanın `tag:` kuralı, gösterilen not kalır). Satırlar geçerli değerle açılır, `Esc` değiştirmeden kapatır. Kapsam satırı süzgeçleri söyler (` · yalnız bağlantılar`, ` · 'uses' ilişkisi`, ` · #proje`).
  - `/` haritada bul: kapsam satırının yerinde `Bul:` satırı; yazdıkça yol ya da `#etiket`i katlanmış metni içeren düğümler (`app::find_map_nodes`) yanar, gerisi söner, ilki panelin ortasına gelir ve Tab seçimi olur; `Enter`/`Tab` sonraki, `Shift+Tab` önceki; sayaç `1/3`; `Esc` kapatır, seçim eşleşmede kalır (`Enter` açar).
  - `R` (route) yol kipi: `yol kaynak → hedef`, `Tab` hedefi notlar arasında seçer, `d` yönlüyü açıp kapar, `Enter` en kısa yolu arar (`app::shortest_path`, süzgeçten geçen bütün notlar üzerinde; yolun düğümleri derinliğin ötesinde de girer). Yol yeşil çizilir, uçları yeşil adlıdır; kapsam satırı ` · yol 2 adım`, alt satır ve bildirim `En kısa yol a → c: 2 adım` ya da `a ile c arasında yol yok`. `Esc` (kip yokken) yolu temizler.
  - `P` (pin) hedefi bulunduğu yere sabitler ya da çözer, `U` (unpin) hepsini çözer; ileti alt satırda.
  - `n` var olan ad satırını açar (liste görünümüne geçer, `create_named_note`); oluşan not haritada kalır ve panelin ortasına sabitlenir; satırdan Esc ile çıkılırsa yer unutulur (sonraki not sabitlenmez). `d` yalnız Tab'la seçilen notu onaysız çöpe atar (işaretler değil; var olan yol), `u` geri getirir; seçim yoksa (süzgeç sildi, `C` kipindeki `d` kenarı kaldırdı) gösterilen not atılmaz, ileti seçmeyi söyler.
- **Sorgu (F3, GUI çubuğuyla eşit; akış `app::MapQuery`, kural SPEC-graph "Sorgu çubuğu"; oturumluk):** `Q` (query; haritada boştu, `:` palettir) kapsam satırının yerinde `Sorgu:` satırını açar; vazgeçilen ya da hatalı sorgunun metniyle açılır. `↑` `↓` (`Ctrl+P` `Ctrl+N`) oturumluk geçmişte gezer (arama geçmişinin yardımcısı, `InputKind::Query`; sır benzeri metin girmez), `Tab` kayıtlı görünüm adlarını listeler (tek aday doğrudan yazılır), `Enter` çalıştırır (kayıtlı görünümün adı görünümü çalıştırır), `Esc` kapatır. Okuma hemen çalışır: sonuçtaki notlar renkli kalır, gerisi söner, döndürülen kenar sarı çizilir; sonuçtaki notlar çevre kapsamında da haritaya girer; kapsam satırı `Q sorgu  O tablo  Esc temizle  Sorgu: 3 satır, 2 not vurgulandı` olur, özet alt satırda da. Yazma önce önizlenir: onay penceresi (`Yazan sorgu çalıştırılsın mı?`) değişiklik sayılarını ve geri alma olmadığını söyler; varsayılan Hayır, `←` `→` seçer, `Enter` onaylar (`y` kısayolu yok: yanlışlıkla yazmayı önler, silme onayıyla aynı kural); `Uygula` liste ve kenarları yeniden okur. Korunan veri, sözdizimi ve sınır hatası onay sormadan satırı metin ve konumlu iletiyle yeniden açar; dar panelde kesilen ileti alt satırda (hata bildirimi) tamdır. `O` (output) sonucu liste panelinde tablo olarak gösterir: başlık, eşit sütunlar (` │ `), yalnız görünen satırlar; `↑` `↓` `PgUp` `PgDn` satır seçer (ters renk), `Enter` satırın ilk not hücresinin notunu açar, `O` haritaya döner. Hücre sonuç kurulurken bir kez `terminal_safe`'ten geçer (satır sonu, bidi, denetim U+FFFD) ve `limits.app.query_cell_bytes`'ta (4 KiB) kesilir; sütuna sığmayanı `…` ile kısalır. `Esc` (satır kapalıyken) önce sorgunun sonucunu ve vurgusunu kaldırır, sonra yolu. **Bilinen sınırlar:** sorgu UI iş parçacığında çalışır, iptal edilemez: en kötü durumda `limits.query.max_duration` (5 sn) kadar arayüz yanıt vermez (adlandırma da bu süreye sayılır); onaylanan yazma iki kez çalışır (önizleme + uygulama); depo yokken yazmanın önizlemesi `graph.db`'yi kurar.
- **Okuma ve CPU:** kenarlar yalnız harita görünürken ve taramada değişiklik görülünce tek okumayla gelir (yalnız ilişki değişse de); kare başına depo çağrısı yoktur. Yerleşim kare başına `TuiState::kMapIterationsPerFrame` (1) adım atar ve hareket sürdükçe kare ister; durunca kare üretilmez.

## Bildirimler (NY-G)

İşlem sonucu (GUI ile aynı olaylar ve model `app::NoticeQueue`, SPEC-gui "Bildirimler") iki alt satırın üstünde sağa yaslı tek satırdır: ters renkli, türün renginde, önekli (`Tamam`, `Hata`, `Bilgi`), en yenisi; yineleme `(×n)`, bekleyen diğerleri `+n bildirim`. Satır `terminal_safe`'tir ve genişliğe sığdırılır; panel yerleşimi kaymaz (bindirme). Aynı metin alt çubukta yinelenmez, kısayol ipuçları kalır. Süre `tuning.notice_ms`; hata sonraki tuşa dek kalır. Düşme anı `render`ın uyanma anıdır (`wake_due`), kare istenmez.

## Pano

- **Arka uçlar** (`src/tui/clipboard.*`, kitaplık yok): Windows Win32 API; `WAYLAND_DISPLAY` varsa `wl-copy`/`wl-paste --no-newline`, `DISPLAY` varsa `xclip -selection clipboard`, yoksa `xsel --clipboard`; macOS `pbcopy`/`pbpaste`. Kabuksuz, veri stdin'den, 1,5 sn süre sınırı (takılan araç UI'yı en çok bu kadar bekletir, sonra öldürülür). Çocuk ortamından `SSL_CERT_*`, `LD_*`, AppImage değişkenleri ve senkron token'ı düşer. Çatallanıp seçimi tutan araç beklenmez: stdout boş aygıttır, ana sürecin çıkışı yeterlidir.
- **Yedek:** araç yoksa ya da başarısızsa kopya OSC 52 ile terminale gider; base64'ten önce en çok 75.000 bayt, üstünde ileti ve yalnız iç pano. Bu sınır config'te değildir: terminallerin OSC 52 dizisi için güvenli sınırıdır (protokol); ileti bunu ve pano aracı (wl-copy, xclip, xsel) kurulunca kalktığını söyler. Araç başarısız olduysa sistem panosu eskidir: sonraki `Ctrl+V` iç panoyu seçer. Araç yokken `Ctrl+V` iç panoyu yapıştırır ve `Ctrl+Shift+V`'yi söyler. Pano hatası çökertmez, metin kaybetmez.
- **Reddedilen veri:** NUL içeren metin kopyalanmaz; NUL ya da geçersiz UTF-8 içeren ve `limits.tui.paste_bytes`'tan (32 MiB; 0 = sınırsız) büyük pano yapıştırılmaz, metin değişmez, neden ve anahtar söylenir.
- **REVIEW fare seçimi:** sürükleme seçer, ters renkli gösterir, bırakınca kopyalar. Uçlar mantıksal konumdur: sarma ve gutter kopyaya girmez, satır sonları dosyadaki gibidir (CRLF). İşaretçinin altındaki karakter seçime dahildir. Panel kenarını geçen fare en yakın hücreye kenetlenir; sürüklerken tekerlek kaydırır, çapa yerinde kalır, uç aynı hücrenin yeni metnine gelir. Tıklama seçmez ve kopyalamaz. Sol tuş bırakılmadan başka olay gelirse (Release kaybı) sürükleme biter ve kopyalar. Geçersiz UTF-8 notta seçim kopyalanmaz, neden söylenir. Tuş ve yeni tıklama seçimi kaldırır; bölücü sürükleme ve liste tıklaması aynı kalır.

## REVIEW biçimli görünüm

`src/tui/markdown_view.*` (md4c, `render_markdown`), `src/tui/data_view.cpp` (`render_csv`, `render_json`), `src/tui/styled_view.*` (sarma, tablo yerleşimi): kaynak baytlar + genişlik -> biçimli satırlar. Saf: ağ, dosya, terminal kaçışı yok; FTXUI'ye yalnız `view.cpp` boyarken değer. Uzantı seçer: `md`/`markdown`, `csv`, `json`; diğer notlarda yalnız ham metin vardır, `R` etkisizdir, etiket `REVIEW` kalır. Ham görünüm satır içinde dikey kaydırmaz: uzun satırın yalnız ekrana sığan kısmı (hücre başına en çok 16 bayt) sarılır, renklendirilir ve boyanır; kare işi satır boyuyla değil görünen hücreyle orantılıdır (NX-B). Satırın ilk ekranından sonrasına yana kaydırılarak gelinir (NX-D; GUI ham önizlemesi uzun satırı 4 KiB'lık satırlara bölüp tümünü gösterir): `Shift+→`/`Shift+←`, `Shift`+tekerlek ve yatay tekerlek (terminal bildirirse) tüm satırları bir panel genişliği kadar baytla kaydırır; sarılan dev satırda bu bir görüntü satırıdır. Ofset bayttır, kod noktası başına çekilir; görünen en uzun satırın son baytını geçmez. Seçim, arama ve bağlantı vurgusu kaymış satırda doğru yerdedir. Renklendirme, yazım denetimi ve eksik `[[bağlantı]]` soluklaştırması görünen kısımdan başlar (satır başından çözümlemek kare işini satır boyuna bağlardı): kesimdeki sözcük yanlış yazılmış, kesilen bağlantı soluk görünmeyebilir, renk bağlamı satır başındaki gibi olmayabilir. Not değişince ofset sıfırlanır; biçimli görünümde (satırlar panele sığar) etkisizdir. Satırın tamamı editörde (`e`) de okunur.

- **Markdown:** başlık düzey işaretiyle (`#`, `##` soluk; her düzey ayrı biçim), kalın, eğik, üstü çizili, satır içi kod, kuşatılmış kod (`syntax` tokenizer'ı dil etiketinden, `tui::fence_language`, GUI önizlemesiyle ortak; `python`, `javascript` gibi tam adlar da, bilinmeyen dil düz camgöbeği), alıntı `│` ile (kaplar dıştan içe: liste öğesindeki alıntının çubuğu öğe metninin hizasında), iç içe sıralı ve işaretli liste (imler derinliğe göre `•` `◦` `▪`, GUI ve PDF ile ortak `pdf::kListBullets`), görev listesi `[x]` `[ ]`, sarılan hücreli tablo (sütun hizası `:--` `:-:` `--:`), HTML5 adlı ve sayısal varlıklar (`core::decode_html_entity`, md4c'nin tablosu), yatay çizgi, bağlantı metin + soluk URL (terminal hyperlink kaçışı yok), görsel `[görsel: alt]`, HTML yazıldığı gibi. Renkler 16 ANSI setinden, aynı `token_styled` paleti.
- **CSV:** ayırıcı `detect` kuralıyla (`detect::table_delimiter`: ilk 200 dolu satır); tırnaklı alan, `""`, tırnak içi satır sonu boşluğa iner. İlk satır başlık. Tutarlı tablo değilse ham metin + not.
- **JSON:** katı RFC 8259 denetiminden geçerse girintilenir, `syntax` JSON renkleriyle; geçersizse ham metin + not. İç içelik sınırı yoktur: yazıcı özyinelemesizdir (açık yığın), derin not yığını taşırmaz (L2; eski 128 sınırı kalktı).
- **Geri düşüş:** görünüm olmadıysa (geçersiz, tablo değil, kullanıcının verdiği `limits.tui.view_bytes` üstü; varsayılan 0 = sınırsız) ham vurgulu metin ve toplamlar satırında soluk neden. Not içerik satırı değildir: kaydırma, fare ve arama eşlemesi bozulmaz. Etiket `ham` der.
- **Güvenilmez içerik:** çıkan her metin `terminal_safe`ten geçer (C0, C1, DEL, bidi ve sıfır genişlikli karakterler, ESC yok; `U+FFFD` olur). Süre girdiyle doğrusal; sınırlar görünürdür ve config'tedir (`tui::ViewLimits`, `view_limits_of`): `limits.tui.view_lines` (100.000), `table_rows` (0: sınırsız; csv dizinli, md tablosu parçalı, NY-P), `table_columns` (128); `0` = sınırsız. Sığmayan sütun ve kesilen satır soluk satırla söylenir (satır panele sığsın diye anahtar adı yazılmaz). İç içelik öneki panelin en çok yarısını alır, ek derinlik girinti katmaz.
- **`R`:** görünüm / ham, oturum boyunca tüm notlarda; `REVIEW · görünüm`, `REVIEW · ham` etiketi (Türkçe katalogda çerçeve adı `İNCELEME`). EDIT her zaman ham metindir. Kaydırma geçişte aynı kaynak bloğunda kalır.
- **Arama (`/`, `n`, `N`):** gösterilen metinde çalışır; biçimli görünümde `**` bulunmaz, `kalın` bulunur. Eşleşme görüntü satırı içindedir: sarmanın böldüğü sözcük bulunmaz. İçerik aramasının (`Tab`, `f`) satır vurguları ham satıra aittir, biçimli görünümde boyanmaz; önizleme yine eşleşen bloğa kayar.
- **Kopya:** fare seçimi gösterilen metni kopyalar (satırlar `\n` ile, tablo kenarları ve liste imleri dahil; geçersiz UTF-8 not da arındırılmış haliyle kopyalanır). `c` her zaman notun özgün baytlarını kopyalar.
- **`e` / `Enter`:** üstte görünen bloğun kaynak satırında EDIT açılır: `RenderedView::source_lines` görüntü satırı -> kaynak satırı eşlemesidir (kod ve tablo satırı başına, diğerleri blok başına). md4c konum vermez; metin geri çağrısının gösterdiği adresten bulunur, metni olmayan blok (çizgi) sonraki dolu satırdır. EDIT'ten dönüş o bloğa iner.
- **Parça parça üretim (NX-D):** 64 KiB'tan büyük md GUI önizlemesiyle aynı kuralla parçalara bölünür (`app::markdown_chunks`); `PreviewCache` yalnız görünen pencerenin bir ekran ötesine dek parça üretir, kaydırdıkça ekler. Parçanın kendi üretimi tek geçişle aynıdır; parçalar arası boş satır ve `view_lines` sınırı sürer, sonuç tümü üretilince tek geçişle aynıdır. Devamı üretilmemiş görünümde kaydırma son satıra kenetlenmez: `G`, not içi arama (`/`) ve üretilmemiş parçadaki kaynak satırına dönüş (EDIT dönüşü, içerik araması) gereken parçaları o karede üretir (`G` ve arama tümünü). Uzayan görünümde seçim ve bağlantı seçimi kalır, arama eşleşmeleri yenilenir. 64 KiB'tan küçük not tek parçadır. Bağlantı başvuru tanımları (`[ad]: url`) bütün notta geçerlidir: GUI ile aynı ön geçiş her parçaya eklenir (kural ve kalan farklar SPEC-gui "Parça parça yerleşim"). Kesim yeri olmayan tek dev blok (tek satır dahil) GUI ile aynı kuralla 128 KiB'ta zorla kesilir; taşınan çit ya da tablo başlığı satırları kaynak satır eşlemesinde parçanın ilk satırına düşer (NY-P). csv ve json da parça parça (NY-P): not bir kez dizinlenir (`app::index_csv`, `app::index_json`, GUI ile ortak), parça `PreviewCache::kDataChunkLines` (2.000) tablo satırı ya da girintili JSON satırıdır (`render_csv_rows`, `render_json_lines`). Tablonun sütun genişlikleri ilk parçadan alınır, sonraki parçalarda geniş hücre sütununda sarılır; kesilen satır ve sütun notu son parçadadır.
- **Önbellek:** `PreviewCache` not sürümü ve genişlik başına bir kez üretir; hızlı gezinmede `throttled` okumayı ertelediği için görünüm de üretilmez. Genişlik değişince yeniden sarılır; art arda değişimlerde önceki yerleşim kalır, `kResizeSettleMs` (120 ms) durunca üretilir, sakin kullanıcının tek değişimi beklemez. Üretim UI iş parçacığındadır, bu yüzden boyut sınırı 50 ms bütçesinden gelir (ölçüm aşağıda). `main.cpp` değişmedi: eşleme `throttled` içinde modele bildirilir.

Ölçüm (T69, Release, 80 sütun, üç koşunun en kötüsü, sınır boyutunda girdi): 2 MiB'te karışık md 155 ms, bağlantı 144, kod 120, JSON 86 ile 50 ms bütçesini aştı; 512 KiB'te hepsi 33 ms altına indi ama `[` yığını 85 ms kaldı. 256 KiB'te en kötü `[` yığını 45 ms, JSON 12, uçtan uca genişlik değişimi (karışık md) 9, diğerleri 10 ms ve altı. Üretimi arka iş parçacığına almak yerine sınır düşürüldü: daha büyük not ham metinle zaten okunur, görünüm iptali ve eski sonuç yarışı eklenmez.

## Bağlantı bölümü

Seçili notun grafı REVIEW'un altındadır (`src/tui/link_panel.*`, saf model). Veri `NoteStore::links`ten gelir (SPEC-graph "Bağlantılar"), önizlemeyle aynı ertelenmiş yüklemede okunur: gezinmede tuş başına depo okuması yoktur, `kCoalesceMs` içinde son seçim tek okuma yapar. Liste ya da not değişince önizlemeyle birlikte tazelenir. Yeniden tarama grafın `data_version`ını da karşılaştırır (`tui::StoreScan`): listeyi değiştirmeyen, yalnız ilişki ya da bağlantı getiren senkron da bölümü tazeler.

```
│# Plan                               ┃
│...                                  ┃
┠4/6 ─────────────────────────────────┨
┃Gelen (1)                            ┃
┃  gelen.md                           ┃
┃Giden (2)                            ┃
┃  hedef.md                           ┃
┃  ikinci.md                          ┃
┃İlişkili (1)                         ┃
┃  ikinci.md                          ┃
┃Eksik (1)                            ┃
┃  [[eski-ad]]                        ┃
```

- **Etiketler (T80):** notun `#etiket`leri (`NoteLinks::tags`, ilk yazılış) ayırıcı satırında `#iş #proje/alt ───` olarak durur, `terminal_safe`ten geçer. Bağlantısız ama etiketli notta bölüm yalnız bu tek satırdır; ikisi de yoksa bölüm yoktur. Tıklama ve tekerlek ayırıcıda bir şey yapmaz, `b` yalnız bağlantı varken odaklar.
- **Satırlar:** ayırıcı, `Gelen (2)`, `Giden (3)`, `İlişkili (1)`, `Eksik (1)`; satırda ikon ve yol. Boş grubun satırı yok, hiç bağlantı yoksa bölüm de ayırıcı da yok: önizleme eski yüksekliğindedir. Editör açıkken ve çöp görünümünde bölüm yoktur. Çerçeve, toplamlar ve ipucu çubuğu değişmez; odak etiketi `REVIEW` kalır.
- **Eksik:** yolu olmayan metin `[[ad]]` soluk çizilir; köşeli ayraç renksiz terminalde de ayırt ettirir. Adı değişen hedef burada görünür: metin yeniden yazılmaz (SPEC-graph).
- **Yükseklik:** panelin üçte biri, en az `kMinRows` (3), en çok `kMaxRows` (12) satır; 60×15'te önizlemeye 6 satır kalır. Taşan bölümde ayırıcıda `4/6` (başlıklar sayılmaz). Yalnız görünen satırlar çizilir, binlerce bağlantıda kare maliyeti değişmez.
- **Tuşlar (`b` ile odak):** `↑` `↓` `k` `j` öğe satırlarında gezer, başlığı atlar; `PgUp` `PgDn` görünen satır kadar; `Enter` nota gider, süzgeç ya da klasör hedefi gizliyorsa `select_note` gevşetir, odak REVIEW; eksik satırda gitmez, nedeni alt satırda; `Esc` `b` REVIEW'a döner; `<` `>` `z` çalışır. Son bağlantı kalkarsa odak REVIEW'a döner.
- **`L`:** Liste, REVIEW ve bölümde boş girdi açar (`İlişkilendir:`, `tui.link_label`); `Tab` not yollarını önerir. `Enter` hedefi doğrular (olmayan not girdide kalır) ve **ad satırını** açar (F1, SPEC-graph "Adlı ilişkiler"): `İlişki a.md — b.md, ad:`; `Tab` var olan adları tamamlar (`relation_names`), boş `Enter` adsız ilişkidir. **Yön:** `Ctrl+D` (Directed) "yönlü"yü açıp kapar, etikette `—` yönsüz, `→` kaynaktan hedefe yönlüdür; kullanıcı `Ctrl+D`ye basmadıkça yazılan ad var olan bir adsa onun varsayılanı gelir (`app::default_directed`: adın ilişkilerinin çoğu yönlüyse), yeni ad yönsüzdür. Seçim harfi değil kontrol tuşudur: ad satırında her harf metindir. `Enter` `app::relate_map_notes`; kendine ilişki, geçersiz ad ve çiftte aynı ad anahtarı hatası satırda kalır, `Esc` yazmaz. Ad güvenilmezdir: etiket, öneri ve hata `terminal_safe`ten geçer. **İlişkili** satırı yön oku, yol ve adı gösterir (`↗ b.md — uses`: ↗ bu nottan çıkan, ↙ gelen; `app::related_row_text`). İlişkili satırda `n` (name) aynı ad satırını ilişkinin adıyla açar (`rename_relation(id)`, boş = adsız, yön sorulmaz), `f` (flip) yönü çevirir (yönsüz ilişki bu nottan çıkan olur), `u` (undirected) yönsüz yapar (`set_relation_direction(id)`); üçü de yalnız o ilişkiyi kimliğiyle hedefler, sonuç bildirimdedir. Bölümde **İlişkili** satırındayken `L` kaldırma onayı açar, varsayılan Vazgeç; onay seçili satırın ilişkisini kimliğiyle siler (`remove_relation(id)`), çiftin öbür ilişkileri kalır; adlı ilişkide onay metni adı söyler. Bölümün diğer satırlarında yeni ilişki girdisi açar. Klasör satırında etkisiz. Başarılı yazma `notify_changed` çağırır. `L` harfi İngilizce "link"ten gelir.
- **Fare:** satıra tıklamak nota gider; başlık ve eksik satır yalnız odaklar, ayırıcı hiçbir şey yapmaz. Tekerlek işaretçi bölümdeyse seçimi gezer, önizlemeyi kaydırmaz. REVIEW fare seçimi bölüme taşmaz.
- **Hostile ad:** yol ve eksik metin çizimden önce `terminal_safe`ten geçer; panelden uzunsa baştan kısalır, dosya adı görünür kalır (`tail_cells`). Model ham baytı tutar.
- **İpuçları ve yardım:** `key_hints_preview`, `key_hints_list`te `b` `L`; bölüm odağında `key_hints_links`; `F1`de "Bağlantılar" başlığı.

## Tamamlama ve yazım denetimi

- **Gölge öneri:** yalnız satır sonunda, yazılan her harften sonra; önek en az 2 bayt. Önce bu nottan imlece en yakın sözcük, sonra diğer notlardan en sık geçen. Her tuş bir önceki öneriyi düşürür, hareket ve `Esc` yeni üretmez. Soluk çizilir, panele kırpılır, sarmaya ve bayta girmez. Bu not imleç çevresinde ±`limits.tui.completion_window_bytes` (256 KiB) taranır: tuş süresi not boyutundan bağımsızdır, daha uzaktaki sözcük önerilmez. En kısa önek `tuning.completion_min_prefix_bytes` (2), en uzun sözcük `limits.tui.completion_word_bytes` (64 bayt).
- **Diğer notlar:** `WordTableBuilder` notları işçide okur, tablo yalnız bellektedir (en çok `limits.tui.completion_table_words` 200.000 sözcük; not başına `completion_note_bytes` 2 MiB, toplam `completion_table_bytes` 64 MiB), indeks dosyası yoktur. İlk editör açılışına dek hiçbir şey okunmaz, kayıttan 1 sn sonra yeniden kurulur. `ContentIndex` kullanılmadı: metni özeldir ve UI iş parçacığına aittir.
- **Sır koruması:** Yalnız satır anahtarları görünür (ad kuralıyla aynı `core::secret_line_key`; değer ve yorum öneriye, tabloya ve başka nota girmez): `env`, `properties`, `ini`, `toml`, `yaml` ve betik ya da yapılandırma tipleri `sh`, `bash`, `zsh`, `fish`, `ps1`, `bat`, `cmd`, `json`, `jsonc`, `json5`, `conf`, `cfg`, `tf`, `xml`, `dockerfile`. Diğer tiplerde düzyazı önerilir ama `KEY=değer` ve `export KEY=değer` satırı yalnız anahtarını verir. Satırı pencereye sığmayan sözcük önerilmez; bir öneri için en çok 64 aday reddedilir: tuş süresi sınırlı kalır.
- **Liste (`Ctrl+Space`):** adaylar bu notun penceresi (en çok `limits.tui.completion_note_words` 2.000) ve tablo, tekil, en çok `limits.tui.completion_candidates` 5.000; `search::fuzzy_match` ile süzülür, ilk 8'i görünür. Seçim önekin yerine tek geri alma adımıyla girer. Terminal Ctrl+Space'i NUL gönderir; FTXUI 7.0.3'te `Event::Custom` de `Special({0})`dır ve olaylar yalnız baytla karşılaştırılır, bu yüzden arka plan çizimleri kendi olayını (`redraw_event`) gönderir.
- **Yazım denetimi:** yalnız `md` ve `txt`; sözlük dilden (`tr` -> `tr_TR`, diğerleri `en_US`), config yok. `hunspell -a -i utf-8 -d <sözlük>`, `LC_ALL=C.UTF-8`. Satırlar `^` önekiyle gider (`*sözcük` ispell komutu olup kişisel sözlüğe yazardı). Çizim önbelleğe bakar, bilinmeyen satırı kuyruğa atar: 250 ms boşta kalınca işçi sorar, yanıt çizimi tetikler. Süreç ilk istekte başlar, nesneyle ölür. Program yoksa, başlamazsa, 2 sn yanıt vermezse ya da ölürse oturum boyunca kapalıdır, yeniden denenmez; `F1` bunu söyler. `limits.tui.spellcheck_line_bytes` (8 KiB) üstü satır denetlenmez. Windows'ta da çalışır (hunspell PATH'teyse; \r\n yanıt kabul edilir). İçerik makineden çıkmaz.

## Dil sunucusu (LSP)

Yalnız EDIT; **varsayılan açık** (kullanıcı kararı 2026-11-02, plan kararını ezer), yalnız kurulu sunucularla. Plan: `tasks/plan-lsp.md`.

- **Ayar:** `[lsp.servers]` LSP `languageId` → kabuksuz argv (`python = ["pyright-langserver", "--stdio"]`) ya da en çok 4 argv'nin listesi (`html = [["vscode-html-language-server", "--stdio"], ["htmx-lsp"]]`); kullanıcının girdileri `defaults.toml`'daki varsayılan sunucuların üstüne tek tek biner, `dil = []` o dili, `enabled = false` hepsini kapatır. Varsayılanlar (python, c, cpp, shellscript, markdown, lua, html + htmx-lsp, css, scss, less, dockerfile, sql) kendi varsayılan ayarında proje kodu çalıştırmayan ve ağa çıkmayanlardır; java (jdtls), go (gopls), rust (rust-analyzer), javascript/typescript (typescript-language-server), yaml, toml (taplo), json bunu gömülü ayarla sağlar ("Sertleştirme", T59e). Komut ve `[lsp.settings.<dil>]` yalnız yerel `config.toml`'dan gelir, not ya da graf verisi ikisini de taşıyamaz. Geçersiz girdi (dizge olmayan argv, NUL, 64 argümandan ya da 4096 bayttan uzun, kimlik `a-z0-9_-` dışı ya da 32 bayttan uzun, 32 dilden ya da dil başına 4 sunucudan fazlası) uyarı olur ve atlanır; diğerleri çalışır. Ayar ekranı bölümü düzenlemez, kayıt korur.
- **Dil:** tek tablo (`lsp_workspace.cpp`, `lsp_language_for`): önce tam dosya adı (`Dockerfile`, `Containerfile`, `Makefile`, `makefile`, `GNUmakefile`, `CMakeLists.txt`), sonra büyük/küçük harf duyarsız uzantı; sözdizimi vurgusundan bağımsızdır. Kimlikler: c, cpp, python, shellscript, markdown, lua, json, jsonc, yaml, toml, javascript, javascriptreact, typescript, typescriptreact, go, rust, java, csharp, html (Thymeleaf/htmx dahil), css, scss, less, sql, dockerfile, makefile, cmake, ruby, php, kotlin, swift, zig, xml, ini; uzantı listesi README'de. `env` ve `properties` tabloda yoktur (sır): ne yansıtılır ne gönderilir. Taslak, salt-okunur ve `limits.lsp.note_bytes` (2 MiB) üstü not gönderilmez ve yansıtılmaz. Sunucu çıktısının korumaları `limits.lsp.*`'dadır (`tui::lsp_limits_of`; varsayılanlar bugünkü değer, `0` = sınırsız, riski defaults.toml'da): çerçeve başlığı `header_bytes` 4 KiB, gövde `body_bytes` 16 MiB, gövdenin JSON iç içeliği `message_depth` 64 ve değer sayısı `message_values` 250.000, belge başına `diagnostics` 1.000, tanı iletisi `diagnostic_message_bytes` 512, öneri cevabı `completion_items` 500, öğenin ekleme metni `completion_insert_bytes` 8 KiB ve etiketi `completion_label_bytes` 1 KiB (aşan öğe atlanır), bekleyen cevaplar `pending_reply_bytes` 1 MiB; süreler `initialize_timeout` 10 sn, `write_timeout` 5 sn, `shutdown_timeout` 1 sn (ardından sabit 500 ms çıkış beklemesi). İşçinin bekleme aralığı (20 ms), çıkış beklemesi ve yazma parçası (64 KiB) iç işleyiştir, config'te değildir.
- **Çalışma alanı:** `state_dir/lsp/workspace/` (0700) sunucunun köküdür. Sunucusu tanımlı dillerin notları `<kök>/<NotePath>` olarak 0600 ve atomik yazılır (notlar arası import çözülsün); kayıt, taşıma/ad değiştirme, silme, çöp, kalıcı silme ve yeniden taramadan (senkron, CLI) 500 ms sonra arka planda eşitlenir; listede olmayan dosya ve boş klasör silinir. Yol bağlar çözüldükten sonra kökün içinde kalmalıdır (`weakly_canonical`); dizine konan bir bağ yalnız kendisi silinir. Çıkışta silinmez, açılışta yeniden eşitlenir; içeriği aynı dosya yeniden yazılmaz. Belge URI'si `file_uri(<kök>/<NotePath>)`.
- **Kurulu olmayan sunucu:** programlar `LanguageServers` kurulurken bir kez `core::find_program_on_absolute_path` ile aranır (yalnız mutlak PATH girdileri: `.` ya da `node_modules/.bin` çalışma dizinine göre çözülürdü; göreli dizinli ad da reddedilir; Windows'ta PATHEXT); bulunan mutlak yol argv[0] olur. Bulunamayan sunucu denenmez; dilin hiç sunucusu yoksa not yansıtılmaz (hiçbiri yoksa dizin oluşmaz) ve o dilde ilk not açılınca bir kez `tui.lsp_not_found` ("pyright-langserver bulunamadı, LSP python için kapalı"); dilin başka sunucusu varsa bir kez `tui.lsp_partly_not_found` ("htmx-lsp bulunamadı, html onsuz çalışıyor"); log yazılmaz.
- **Süreç:** sunucu başına bir `LspClient` (anahtar: dil + listedeki sıra), o dilde bir not EDIT'te ilk açılınca başlar; belge, değişiklik ve kapanış dilin her sunucusuna gider. Çöken ya da başlamayan sunucu oturum boyunca kapalıdır, nedeni sunucu adıyla bir kez alt satırda görünür (`tui.lsp_stopped`: "pyright-langserver durdu: çöktü ya da kapandı"); dilin öbür sunucuları sürer. Dilde birden çok sunucu varsa tanılar birleşir ve iletinin başına sunucu adı gelir (`htmx-lsp: …`). İşçi bildirimleri tek çizim isteğinde birleşir: sonraki kareye dek en çok bir `redraw_event`.
- **Sertleştirme (T59e):** `server_settings.cpp`'deki tek tablo, program adıyla (argv[0]'ın küçük harf, uzantısız dosya adı) seçilir; sunucuya `initializationOptions`, `workspace/configuration` cevabı (noktalı bölüm yolu: "http.proxy" → settings.http.proxy, bölümsüz öğe tamamı, olmayan null; Null ayarda her öğe null), `initialized`'dan hemen sonra ve belgelerden önce `workspace/didChangeConfiguration`, parametresiz bildirimler ve ortam değişkenleri verir. rust-analyzer: `cargo.buildScripts.enable=false`, `procMacro.enable=false`, `checkOnSave.enable=false` (2023 öncesi sürümler de okur), `cargo.noDeps=true`, `CARGO_NET_OFFLINE=true`, `RUSTUP_AUTO_INSTALL=0`. gopls: `codelenses` hepsi false, `vulncheck="Off"`, `GOTOOLCHAIN=local`, `GOPROXY=off`, `GOVCS=*:off`, `GOFLAGS=-mod=readonly`, `CGO_ENABLED=0`. jdtls: Maven/Gradle içe aktarma, wrapper, kaynak indirme, anotasyon işleme kapalı, `updateBuildConfiguration="disabled"`; argv'ye `-data state_dir/lsp/jdtls-data` (0700; argv'de `-data` varsa eklenmez; açılamazsa sunucu başlamaz). typescript-language-server: `disableAutomaticTypingAcquisition=true`. yaml-language-server: `schemaStore.enable=false`, `kubernetesCRDStore.enable=false`, `schemas={}`, `yaml/registerContentRequest` (http(s) şeması `vscode/content` ile istemciye sorulur, -32601). vscode-json-language-server: `handledSchemaProtocols=["file"]`, `json.validate.enable=true`. taplo: `schema.enabled=false`, `catalogs=[]`, `taplo.configFile.enabled=false`, bildirimde yalnız `evenBetterToml` bölümü; komutta `--no-auto-config`. htmx-lsp ayarsız. Kullanıcının `[lsp.settings.<dil>]` JSON'u gömülü ayarın üstüne anahtar anahtar biner (nesneler birleşir, diğer değerler değişir); tabloda olmayan sunucuya yalnız kullanıcı ayarı gider. Kaynaklar `server_settings.cpp` yorumlarında; README "Sertleştirme".
- **Ağ yolu (T59f, T59g):** yalnız Windows'ta (`kBlockNetworkPaths`; SMB/NTLM yalnız orada, Linux'ta `//host/x` yerel yoldur) metninde `references_network_path`'e takılan not yansıtılmaz ve hiçbir sunucuya gitmez; açık nota böyle bir yol eklenirse değişiklik gönderilmez, belge kapanır. Kural anahtar ya da dosya biçimine bakmaz; metin ham ve JSON kaçışları (`\\`, `\/`, ASCII'ye inen `\uXXXX`) ile yüzde kodlaması çözülmüş hâliyle taranır. Ağ yolu: iki ayırıcı (`/` ya da `\`, karışık olabilir), önünde `:` ya da ayırıcı yok, ardından host karakterleri (harf, rakam, `-._`, ASCII dışı bayt), tek ayırıcı ve ayırıcı/boşluk/tırnak olmayan bir karakter; ya da sözcük sonu olmayan `file:` + iki ayırıcı + `localhost` olmayan host, `file:` + dört ve daha çok ayırıcı. Bilinen yanlış pozitif: protokolsüz URL. Not o hâliyle ilk engellendiğinde bir kez `tui.lsp_remote_reference`. Sonuç yol, tampon kuşağı (`EditorSession::buffer_generation`) ve `edit_count` başına bir kez hesaplanır; aynı not için yeni tampon belgeyi kapatıp yeniden açar.
- **Çalışma dizini ve başlangıç (T59f):** sunucular `state_dir/lsp`'de başlar (POSIX `posix_spawn_file_actions_addchdir_np`, Windows `lpCurrentDirectory`; Windows'ta ayrıca `NoDefaultCurrentDirectoryInExePath=1`). İlk yansıtma eşitlemesi (eski dosyaların silinmesi dahil) tamamlanmadan hiçbir belge açılmaz, sunucu başlamaz; bitince işçi çizim ister.
- **Yansıtılmayan notlar (T59e):** adı (harf duyarsız) `rust-analyzer.toml`, `rust-toolchain.toml`, `rust-project.json`, `taplo.toml` olan ve yolunda `node_modules` klasörü geçen notlar çalışma alanına yazılmaz: sunucunun ayarını ezer ya da çalıştırdığı kodu sağlarlar. Karşılaştırma Unicode'un ASCII'ye katladığı ſ, ı, İ, K (Kelvin) ve tam genişlik ASCII'yi de katlar (T59f); yazılan dosya ya da açılan klasör dosya sistemine göre (`fs::equivalent`) engellenen adın kendisiyse silinir ve atlanır.
- **Eşitleme:** tam metin `didChange`, son düzenlemeden 250 ms sonra (yazım denetimiyle aynı tempo), belge başına artan sürümle (`EditorBuffer::edit_count`). Beklerken kare istenmez: gönderim anı (`LanguageServers::Update::send_at`) uyanma anına girer, o anki tek çizim gönderir.
- **Tanılar:** her karede güncel tampona yeniden kısılır (sunucu eski sürümü görmüş olabilir; ters aralık boşa iner, sıfır genişlik bir karaktere açılır). Alt çizgi ve önem rengi: hata kırmızı, uyarı sarı, bilgi açık mavi, ipucu soluk; arama ve seçim önüne geçer. İmlecin üstündeki (ya da hemen sonundaki) tanının iletisi ileti satırında, `terminal_safe` ve tek satır; kayıt ve hata iletileri önce gelir.
- **Öneri:** `Ctrl+Space` listeyi hemen sözcüklerle açar; bekleyen metin gönderilir ve dilin her sunucusuna istek gider. Her cevap geldikçe sunucu önerileri listenin başına eklenir (aynı etiket bir kez; sonraki cevap öncekinin önerilerini silmez), seçili aday seçili kalır. Liste kapandıysa, yeniden açıldıysa ya da belge ilerlediyse cevap düşer. Etiket `terminal_safe` ile gösterilir; ekleme metni yalnız tek satır ve denetim/bidi karaktersizse kullanılır, değilse temiz etiket eklenir.
- **Gizlilik:** yapılandırılmış dillerin notları çalışma alanına yazılır ve sunucuya gider; sunucu ağ kullanıyorsa içerik makineden çıkabilir. Sunucu metni güvenilmezdir: boyut sınırları `core/lsp_protocol.hpp`'de.
- **Kod çalıştırma riski (kullanıcı kararı 2026-11-02: tüm notlar yansıtılmaya devam eder, uyarı verilir):** yapılandırılmış dilin her notu, senkronla ya da `graph import` ile gelenler dahil, sunucunun çalışma alanına girer. Proje dosyalarını çalıştıran sunucu (rust-analyzer `build.rs`/`cargo`, eslint `eslint.config.js`) böylece senkronla gelen bir notun kodunu çalıştırabilir. Varsayılan sunucular bunu kendi ayarıyla ya da gömülü sertleştirmeyle önler; kullanıcı `[lsp.settings]` ile geri açarsa risk onundur (README uyarısı). Program adı tanınmayan sarmalayıcıyla (`["env", "rust-analyzer"]`) başlatılan sunucuya sertleştirme gitmez.
- **Bağlar:** kök ya da üstündeki `lsp/` sembolik bağ ya da junction ise yansıtma oturum boyunca kapanır (`lsp_mirror_refused`), hedefe dokunulmaz. Yol bileşenleri ve eski dosya taraması `core::stat_file`/`core::scan_files` ile yapılır: Windows'ta ad vekili reparse point bağ sayılır, içine inilmez, bağın kendisi de silinmez. `.` ile başlayan girdiler (sunucu önbelleği, ör. clangd `.cache/`) silinmez; not adı `.` ile başlayamaz.
- **Kayıt:** `save_config` `[lsp]`'yi dosyada o an nasılsa öyle geri yazar; uygulama açıkken dosyadan kapatılan LSP ayar kaydında geri açılmaz.
- **Kapsam dışı:** hover, biçimlendirme, tanıma git, yeniden adlandırma, GUI, otomatik indirme.
- **Ölçüm (2026-11-02, Debug, 200 baytlık 2.000 not):** ilk yansıtma 564 ms, değişmeyen ikinci 63 ms, yeni süreçte 186 ms; hepsi işçide, UI'nin bedeli < 1 ms. Tuş başına (Release, 2 MiB python, her tuşta tam metin gönderimi, tuş + eşitleme + çizim): ortanca 0,72 ms, en kötü 7,4 ms (kabul 22).

## Sözdizimi vurgusu

REVIEW ve EDIT'te; tablo güdümlü, bağımlılıksız satır ayrıştırıcı (`src/tui/syntax.*`). Dil uzantıdan (bilinmeyen tip, `txt`, `csv` düz), kaydedilmemiş taslakta canlı `detect`ten gelir: metnin 1/32'si kadar değişiklikte bir yeniden algılama, yani küçük metinde her tuş, büyük metinde seyrek.

- **Diller:** java, cpp, c, py, sh, js, ts, go, rust, sql, json, css, html, xml, yaml, toml, ini, env, properties, dockerfile, makefile, md. Yorum, dizge, sayı, anahtar sözcük, tip/sabit; env/properties/ini/yaml/toml'da anahtar ve değer; xml/html'de etiket; md'de başlık, kod, vurgu.
- **Durum:** çok satırlı yorum ve dizge satırlar arasında sürer; `SyntaxStateCache` satır çıkış durumunu yalnız istenen satıra dek hesaplar, düzenleme o satırdan sonrasını düşürür (`EditorBuffer::take_changed_line`). Boyama yalnız görünen satırdadır. Bilinen sınır: heredoc ve yaml blok metni ayrı tanınmaz, satır satır boyanır.
- **Eşikler:** `limits.tui.highlight_bytes` (2 MiB) üstü not ve `limits.tui.highlight_line_bytes` (64 KiB) üstü satır düz metindir (satır durumu olduğu gibi geçirir); `0` = sınırsız. Durum taraması zaten tembeldir (yalnız görünen satıra dek, düzenleme o satırdan sonrasını düşürür): sınırsızda bedeli büyük notta sona ilk atlayıştır. Tarama her dalda en az bir bayt tüketir ve geri dönmez; kapanışı olmayan ayraç bir daha aranmaz: süre doğrusaldır.
- **Renkler:** tek aile tablosu; TUI `token_styled` (16 ANSI) ve GUI `token_color` (tema token'ı, `src/gui/theme.hpp`) aynı aileyi kullanır. Yorum renksiz ve soluktur ki içerikten parlak görünmesin; `NO_COLOR`'da yalnız soluk ve kalın kalır. Sarı aramaya ayrılmıştır (arama kalın sarı): bu yüzden sayı ailesi TUI'de sarı değil açık kırmızıdır.

  | Tür (`TokenKind`) | Aile | TUI | GUI token |
  |---|---|---|---|
  | Comment | soluk | `dim` | `comment` |
  | String, Value | yeşil | Green | `string` |
  | Number, Constant | turuncu | RedLight | `number` |
  | Type, Code | camgöbeği | Cyan | `type` |
  | Keyword, Key, Tag | eflatun | Magenta | `keyword` |
  | Heading | mavi, kalın | BlueLight + `bold` | `heading` |
  | Emphasis | kalın | `bold` | metin rengi |
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
markalı kümeler de vardır; birleşik dosyanın lisansı üstten belirlenemiyor. Kullanıcı kararı: GUI yalnız tablodaki glifleri (dosya ikonları ve GUI eylem ikonları)
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
  - Sapma (T23): `Kaydet` düğme satırında `Enter` ile kaydedilir, `Esc` çıkar; değişiklik (yazılmış token dahil) varsa GUI'nin Ayarlar'dan çıkış sorusu: `Kaydet` / `At` / `Vazgeç` (`app::CloseChoice`; oklar ve Tab döner, varsayılan ve soruda `Esc` "Vazgeç": formda kalınır, hiçbir şey yazılmaz). `Kaydet` Kaydet düğmesinin akışıdır: doğrulama hatası formda satır içi kalır, ekran kapanmaz. `At` formu kapatır; yazılmış token kasaya gitmez (NX-B). Ayarlar açıkken `Ctrl+C` (çıkış) değişiklik varsa hemen çıkmaz, aynı soruyu sorar (GUI pencere kapanışı gibi): `At` çıkar, `Kaydet` kaydedip çıkar (hata varsa formda kalınır), `Vazgeç` formda kalır; soru açıkken `Ctrl+C` soruyu açık tutar. Sapma (T28): token satırı 12 config satırının ardında (Kaydet düğmesinden önce) ve yalnız https remote iken görünür; `Enter` yazılan token'ı kasaya kaydeder, `Ctrl-D` siler, `Kaydet` yazılmış ama kaydedilmemiş token'ı da `enable`'dan önce kasaya yazar. `enable` akışı senkron açılırken ya da açıkken remote değişince çalışır; origin farklıysa "değiştirilsin mi?" onayı (varsayılan "Vazgeç"), hata formda satır içi. Kullanıcı `config.toml`'u bozuksa kaydedilmez (`note config set` ile aynı kural).

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
21. Komut paleti: `tests/tui/command_palette_test.cpp` ve `e2e_editor.sh`: tablodaki her tuş palet ve F1 çiziminde; her çalışabilir komut tuşuyla aynı `Intent` ve model durumu; çalışamayan soluk, nedenli, çalışmaz; son kullanılan önce ve kalıcı; `:` EDIT ve girdilerde metin; hostile ad ve süzgeç terminale ham gitmez.
20. Liste: `tests/tui/list_ui_test.cpp`: argümanlar, bozuk argümanın yeri, `f`, `d` `u` `D` `T`, `Alt+ok`, `v`, `o`, kalıcılık, `terminal_safe` (C0, C1, bidi), senkron durumlarının işçiden gelmesi.
22. Dil sunucusu: `tests/tui/language_servers_test.cpp` ve `lsp_workspace_test.cpp` sahte sunucuyla: sunucusu kurulu olmayan varsayılan ayarda süreç, yansıtma ve dizin yok, dil başına tek ileti; göreli PATH girdisindeki sunucu başlamaz, mutlak olan başlar; kapalı ya da boş ayarda hiçbir şey; dil tablosu (ad, uzantı, `env`/`properties` yok); kök ya da `lsp/` bağsa yansıtma reddedilir, hedefe dokunulmaz; nokta önbellekleri silinmez; sunucu ayar dosyaları ve `node_modules/` yansıtılmaz; alt çizgi ve ileti satırı (render); iletideki ESC/BEL/bidi terminale gitmez; öneri listeye girer, eski cevap düşer, ekleme kuralları; çökme bir kez bildirilir; `env`/`properties` ne yansır ne gider; tanı seli tek çizim ister; aralıklar güncel metne kısılır; yansıtma kayıt, taşıma, silme, çöp, kalıcı silme ve dış yazmayı izler, 0600/0700, bağla dışarı çıkmaz. T59e (`record` kipli sahte sunucu, program adıyla kopyalanır): her sertleştirilmiş sunucunun anahtarları, ortamı ve argv'si sunucuya ulaşır; kullanıcı ayarı anahtar anahtar biner; tanınmayan sunucuya ayar gitmez; varsayılandaki her riskli sunucu tabloda; html'in iki sunucusu belgeyi alır, tanılar etiketli birleşir, öneriler birleşir; biri çökünce öbürü sürer; eksik ikinci sunucu bir kez bildirilir. Ayar: `tests/core/config_test.cpp`. Tuş başına ≤ 50 ms, 2 MiB python, her tuşta gönderim (`editor latency: a language server`).

**Elle:** Windows Terminal + Fedora (GNOME Terminal) — Türkçe karakter girişi/görünümü, `edit`/`micro`/`nano`/`vim`
ile askıya alma/geri dönme, pencere yeniden boyutlandırma.

## Kapsam dışı
LSP'de tanı ve öneri dışındakiler (bkz. "Dil sunucusu"), eklenti, çoklu imleç, makro, takas dosyası; fare desteği (FTXUI varsayılanı dışında; sağ tık satır menüsü ve [[bağlantı]] tıklaması var), tema seçimi.

## Değişiklik 2026-11-02 (LSP)

Dil sunucusu (T59c); kararlar kullanıcının: tanı ve öneri, Windows, notlar çalışma alanına yansıtılır.

- **ADDED** Bölüm "Dil sunucusu (LSP)", kabul 22; `[lsp]` ayarı (SPEC-core "Config").
- **REMOVED** "Kapsam dışı"ndan LSP; tanı ve öneri dışındakiler kapsam dışı kalır.
- **MODIFIED** (kullanıcı kararı) LSP varsayılan açık, güvenli varsayılan sunucularla; kurulu olmayan sunucu için süreç ve yansıtma yok, tek ileti. Dil kimliği sözdizimi katmanından ayrıldı, tek tablo; daha çok tip ve dosya adı. `[lsp.servers]` varsayılanların üstüne biner, `[]` kapatır.

## Değişiklik 2026-11-02

Komut paleti (T79); kararlar kullanıcının.

- **ADDED** Bölüm "Komut paleti"; `Intent::ToggleIcons`; `recent_commands`.
- **MODIFIED** Liste, REVIEW ve çöp harfleri tek tablodan çözülür (`commands.cpp`); `kList*Letters`, `kTrashLetters`, `kPreview*Letters` kalktı. Davranış aynıdır, kabul testleri değişmeden geçer.
- **MODIFIED** İpuçları `: komut` ile başlar; 60 sütuna sığsın diye `Space detay` `Space aç`, `v column` `v col`, `Space details` `Space open`; klasör ipuçlarında `d` 60 sütundan düşer.
- **MODIFIED** Girdi metni çizimde `terminal_safe`ten geçer: `F2` bidi taşıyan adla dolu açıldığında ham gidiyordu.

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

## Değişiklik 2026-11-03 (T59e)

Dil sunucusu sertleştirmesi ve dil başına çok sunucu; kullanıcı kararı: riskli sunucular varsayılanda açık, sertleştirilmiş.

- **ADDED** "Sertleştirme" ve "Yansıtılmayan notlar"; `[lsp.settings.<dil>]`; `tui.lsp_partly_not_found`.
- **MODIFIED** Varsayılana java, go, rust, javascript, typescript, yaml, toml, json ve html'in ikinci sunucusu htmx-lsp eklendi.
- **MODIFIED** Dil başına birden çok sunucu: belge her birine gider, tanılar etiketli birleşir, öneriler birleşir, durum sunucu başına.
- **MODIFIED** `workspace/configuration` artık her öğeye null değil, sunucunun ayarından cevap verir (rust-analyzer null cevabı istemci ayarını varsayılana sıfırlardı).
- **MODIFIED** Sunucu programı yalnız mutlak PATH girdilerinde aranır.

## Değişiklik 2026-11-03 (T59f)

T59e'nin güvenlik incelemesi.

- **ADDED** "Uzak dosya başvurusu", "Çalışma dizini ve başlangıç"; `tui.lsp_remote_reference`.
- **MODIFIED** Engellenen ad karşılaştırması Unicode katlamasını ve dosya sisteminin kendi eşdeğerliğini de kullanır.
- **MODIFIED** rust-analyzer'a `checkOnSave` nesne biçiminde gider (`{"enable": false}`), eski sürümler bool'u yok sayıyordu.

## Değişiklik 2026-11-03 (T59g)

T59f'nin yeniden incelemesi.

- **MODIFIED** "Ağ yolu": anahtara bağlı denetim (`$schema`, `#:schema`, `extends`) kaldırıldı, tek kural bütün metne uygulanır; yalnız Windows'ta.
- **MODIFIED** Tarama önbelleği tampon kuşağını da anahtar alır: yeniden açılan notun yeni tamponu `edit_count`'u 0'dan saydığı için senkronla değişmiş metin eski temiz sonuçla gidiyordu.

## Değişiklik 2026-11-03 (A1)

Ortak komut kaydı (plan-parity A1).

- **MODIFIED** Komut kimliği, etiketi, bağlamı ve `needs`'i `src/app/commands.hpp`'deki tek kayıttadır; TUI tablosu yalnız tuşu, görünümü ve eylemi taşır. Etiketler `[tui] cmd_*`tan `[command]` bölümüne taşındı (`tui.cmd_why_*` nedenleri kaldı). Palet sıralaması ve son kullanılanlar `app::rank_palette` / `app::remember_command`. Davranış aynıdır, palet testleri değişmeden geçer.
- **ADDED** Eşitlik bekçisi (`tests/tui/command_parity_test.cpp`, GUI'de eşi): kayıttaki her komut TUI'de bağlı ya da `tests/support/command_parity_gaps.hpp`'de sahipli eksiktir.

## Değişiklik 2026-11-03 (A3)

- **MODIFIED** Çöp, `u` yığını, geri getirme, kalıcı silme ve onay kuralı `app::TrashOps`'tadır (GUI ile ortak, `src/app/trash_ops.hpp`); davranış aynı. Notlu klasörün içeriği ve onaydaki sayılar listeden (`app::Listing`) gelir.
- **ADDED** `limits.app.trash_undo_steps` (varsayılan 0 = sınırsız): `u` yığınının derinliği; dolunca en eski adım düşer (not çöpte kalır, `T` ile geri getirilir).

## Değişiklik 2026-11-03 (L2 + L5)

Kaynakta sabit sınır yok (tasks/plan-limits.md). Varsayılanlar ve davranış aynı.
- **MODIFIED** Biçimli görünüm, sözdizimi, yazım, tamamlama, yapıştırma, arama geçmişi, geri alma, taşıma diyaloğu önerileri ve açılıştaki geçiş dışı dosya ipucu `limits.tui.*`'dan (SPEC-core "Sınırlar"; `0` = sınırsız); en kısa tamamlama öneki `tuning.completion_min_prefix_bytes`. Dil sunucusu korumaları ve süreleri `limits.lsp.*`. Modüllerin kod varsayılanları defaults.toml ile aynıdır, bir test denetler.
- **REMOVED** Biçimli JSON'un 128 iç içelik sınırı: yazıcı özyinelemesiz.
- **REMOVED** Kopyalamada (`y` `p`) 1000 ad denemesi: not kopyası adı depo seçer (`-2`, `-3`…), klasör kopyası boş adı sınırsız arar.
- **MODIFIED** Açılış ipucu ve `note graph status`/`migrate --new` deponun config'teki klasör yolu sınırını kullanır. Klasör ya da çöp görünümüne geçiş son geçerli aramayı unutur: sonraki bozuk sorgu eski aramanın satırlarına dönmez.
- **MODIFIED** OSC 52 kopya iletisi sınırın terminal protokolünden geldiğini ve pano aracıyla kalktığını söyler.
- Tembel (görünür pencere) biçimli görünüm üretimi yapılmadı: görünüm, arama ve seçim tüm belgenin satır eşlemesine dayanır; sınırlar config'e taşındı. Sıcak yol değişmedi (yalnız parametre geçişi), perf yeniden ölçülmedi.

## Değişiklik 2026-11-03 (A5)

Ortak belge modeli (plan-parity A5, `src/app/document.hpp`).

- **MODIFIED** Salt-okunur sınıflaması, BOM, kayıt, çakışma seçenekleri (`app::ConflictChoice`, varsayılan kopya), kopya ve taslak oluşturma (`create_exact`, baytlar aynen), dış değişiklik kararı (`app::disk_change`) ve taslak adı (`app::parse_draft_name`) `EditorSession`'dan `app::Document`'a taşındı; GUI aynı kodu kullanır. Salt-okunur nedenlerinin metni `[tui] editor_readonly_*`tan `[document] readonly_*`a taşındı.
- **ADDED** Otomatik kayıt (`tuning.autosave_ms`, plan-parity karar 3), bkz. "Gömülü editör". Soru açmaz, not yaratmaz.
- **MODIFIED** Yeni notun klasörü GUI ile tek kural: vurgulu klasör satırı (önceden `default_folder`'a düşerdi), girilen klasör, seçili notun klasörü, `default_folder`.

## Değişiklik 2026-11-03 (T1 + T2)

TUI'de yalnız GUI'de olan özellikler (plan-parity T1, T2; karar 9 ve 10). Eşitlik izin listesinden TUI'nin `save`, `folder.new`, `export.pdf`, `view.tree`, `view.details`, `links.toggle` satırları çıktı; bekçi bunları kanıtlar.

- **ADDED** `save`: editörün `Ctrl+S`'i tablodadır; palet başka görünümde "yalnız editörde" der.
- **ADDED** `links.toggle` (palet): REVIEW'un bağlantı bölümünü gizler/gösterir; gizliyken `b` soluktur. Oturumluk.
- **ADDED** `view.tree` / `view.details` (palet): Ayrıntılar yalnız geçerli klasörün doğrudan çocuklarıdır (`app::Listing::folder_rows`), ağaç açık klasörlerle döner. Oturumluk.
- **ADDED** `folder.new` (palet, klasör menüsü): `a` ad satırı `alt/` + `/` ile dolar, hedef GUI ile aynı kural (`app::new_note_folder`).
- **ADDED** `export.pdf` (REVIEW `P`): alt satır `<ad>.pdf` önerir, göreli yol çalışma dizinine göredir (CLI gibi); PDF diskteki baytlardan `noteapp_pdf` ile üretilir. Var olan dosya (kırık bağ dahil: `symlink_status`) yalnız aynı yola ikinci Enter'la ezilir; ikili not reddedilir.
- **ADDED** Ayarlar: Kaydet'ten önce "Bağlantıyı test et" yuvası (GUI ile aynı akış: form uygulanır, işçide `app::run_connection_test` = `enable` + tek `sync_now`; config ve kasa yazılmaz, yazılmış token Kaydet ya da Enter ile saklanır). Uzak adres uyuşmazlığı testte sorulmaz, neden sonuçta yazar; Kaydet sorar. Yazılmış token remote başka host'a dönünce silinir (başka sunucuya gitmez).
- **ADDED** REVIEW'da `[[wiki]]`: Tab/Shift+Tab görünen bağlantılar arasında gezer, Enter ya da tıklama hedefi depo kuralıyla (`resolve_link`) çözüp notu REVIEW'da açar; hedefi olmayan bağlantı soluk çizilir ve "böyle not yok" denir. Satırı aşan (sarılmış biçimli satırda bölünen) bağlantı tanınmaz.
- **ADDED** Liste satırına sağ tık: GUI'nin satır menüsündeki eylemler (not: taşı, çöpe, sil; klasör: yeni klasör, yeniden adlandır, çöpe) o satır için sınırlı palet olarak açılır; menü klavyeyle seçilir. "Yeni sekmede aç" sekmelerle (T3) gelir.
- **FIXED** Klasörü kendi üst klasörüne yapıştırmak `InvalidPath` veriyordu; yank/yapıştır/taşı ve `a` adı artık GUI ile ortak `app::item_ops`'tadır, klasör yanına `-2` gelir.

## Değişiklik 2026-11-04 (T3)

- **ADDED** Editör sekmeleri (bkz. "Sekmeler"): `tab.new` `Ctrl+T`, `tab.close` `Ctrl+W`, `tab.next` `Alt+N`/`Ctrl+PgDn`, `tab.previous` `Alt+P`/`Ctrl+PgUp`, `tab.open` (satır menüsü, palet), editörde `Ctrl+K`. Eşitlik izin listesinden TUI'nin `tab.*` satırları çıktı. Neden: TUI–GUI eşitliği (karar 8).
- **ADDED** `limits.app.tabs` (0 = sınırsız); GUI'nin sabit 32 sekme sınırı kalktı.

## Değişiklik 2026-11-04 (T4)

- **ADDED** Not haritası (bkz. "Harita"): `view.map` (palet), braille tuvali, derinlik / bütün çizge / etiket kapsamı, ok / `+` `-` `0` kamera, Tab ile seçim, Enter ile açma, aramada vurgu. Eşitlik izin listesinin TUI yarısı boşaldı. Neden: TUI–GUI eşitliği (karar 8).
- **ADDED** `limits.app.map_nodes` (varsayılan 2.000, 0 = sınırsız): haritanın düğüm sınırı iki arayüzde aynı; GUI'nin sabit `kMaxMapNodes`'u kalktı.
- **CHANGED** Yerleşim (`graph_layout`) ve harita modeli (`note_map`, `MapModel`) `src/gui`'den `src/app`'e taşındı; GUI aynı modeli çizer.

## Değişiklik 2026-11-04 (dosya içe alma)

- **ADDED** Palet `import` (Listede): altta dosya sistemi yolu girdisi (göreli yol çalışma dizinine göre; not tamamlaması yok), hedef `n` ile aynı klasör (`app::new_note_folder`). `Enter` `app::import_paths`'ten geçer (`note import` kuralı, SPEC-cli "Dosya içe alma"): özet alt satırda, ilk not REVIEW'da; hiçbir şey alınamazsa neden girdide kalır, `Esc` kapatır.
- **ADDED** Komut satırı: `notes import YOL... [--folder K] [--name AD] [--overwrite]` TUI açılmadan önce raporu terminale yazar (CLI'nin satırları), sonra TUI'yi ilk notla REVIEW'da açar; hiçbir şey alınamazsa TUI açılmaz, çıkış 1; kullanım hatası 2. `notes --help` kısa kullanım. Argümansız açılış değişmedi.

## Değişiklik 2026-11-05 (MAP-R)

- **ADDED** Haritada ilişki kurma (bkz. "Harita" → "İlişki kurma"): `C` kipi (Tab hedef, Enter, `r` `l`, `d`, Esc), düğümden sağ sürükleme (terminal geçirirse Shift+sol), kenara sağ tık + `d`. Kural GUI ile ortak (`app::map_relations`); komut kaydı değişmedi, eşitlik listesi boş kalır. Neden: TUI–GUI eşitliği, GUI'deki fareyle ilişki kurma (kullanıcı isteği).

## Değişiklik 2026-11-05 (U2 T3)

- **MODIFIED** Üst satır: yol adları kalın, `/` soluk; sağ grup soluk ve ` · ` ayraçlı; senkron durumu ikon ve renkle (`StatusLine::sync_state`). Neden: plan-ui U2 T3, durum bir bakışta okunsun.
- **ADDED** Alt satırın başında kip hapı (`NO_COLOR`'da ters). Neden: renksiz terminalde de kip okunur.
- **MODIFIED** Kısayol çubuğu: tuş kalın ve kip renginde, açıklama varsayılan renkte (önceden tüm satır soluktu); `F1 yardım` sağa sabit. Listede, klasörde ve çöpte `? yardım` ipucu `F1 yardım` oldu (`?` yine çalışır); `< >` `</>`, `PgUp PgDn` `PgUp/PgDn` yazılır. 60 sütunda F1'in yerini şunlar bırakır: Türkçe listede `f ara`, klasör satırında `F2` (70 sütundan), çöpte `Esc/q geri` (80 sütundan). Süzgeç yanında korunan ipucu beşten dörde indi.

## Değişiklik 2026-11-05 (U2 T5)

- **MODIFIED** Palet satırı `move — Taşı  m` yerine `Taşı … move  m`: etiket solda, kimlik ve tuş sağda soluk. Süzme ve sıralama değişmedi. Neden: plan-ui U2 T5, GUI paletiyle aynı sıra; satır okunan adla başlar.

## Değişiklik 2026-11-05 (NX-B)

- **MODIFIED** Ayarlar ekranında değişiklikle `Esc`: "Değişiklikler atılsın mı?" (At / Vazgeç) yerine GUI ile aynı Kaydet / At / Vazgeç sorusu (`app::CloseChoice`), varsayılan Vazgeç. `tui.settings_confirm_discard` yerine `tui.settings_confirm_leave`. Neden: iki arayüzde aynı çıkış akışı.
- **ADDED** `limits.app.map_depth` (varsayılan 6, 0 = sınırsız): harita derinlik tuşları `1`–`9` bu sınıra kadar, ipucu satırı `1-{n}`; sabit `MapOptions::kMaxDepth` kalktı, GUI adımlayıcısıyla aynı sınır. Neden: plan-limits L1.
- **FIXED** Ayarlar kirliyken `Ctrl+C` sormadan çıkıyordu; artık Kaydet / At / Vazgeç sorar (`SettingsState::ask_before_quit`). Ayarlar'da Kaydet `limits.app.map_nodes`/`map_depth`'i dosyadan çalışan haritaya uygular.

## Değişiklik 2026-11-05 (NX-C içe alma arka planda)

- **MODIFIED** Palet `import` Enter: içe alma işçide koşar (`app::ImportJob`, GUI ile aynı), girdi kapanır; ilerleme durum satırında `İçe alınıyor n/m: <yol>  Esc durdurur` (yol `terminal_safe`). Sürerken `Esc` (Liste, Önizleme, Bağlantılar) ve `Ctrl+C` (editör dışında; uygulamayı kapatmaz) durdurur: alınanlar kalır, özet durdurulan girdi sayısını söyler. Hiçbir şey alınamazsa girdi yazılanla ve nedeniyle geri açılır (başka girdi açık değilse ve depo değişmediyse: yalnız klasör kurulduysa yeniden deneme `-2` kopyası kurardı). İlk not REVIEW'da yalnız odak editörde değilse açılır. Biri sürerken ikincisi girdide reddedilir. `notes import YOL...` komut satırı eşzamanlı kaldı.

## Değişiklik 2026-11-05 (NX-D)

- **MODIFIED** Büyük md biçimli görünümü parça parça üretilir (bkz. "REVIEW biçimli görünüm"); önceki "Tembel biçimli görünüm üretimi yapılmadı" notu kalktı. Ölçüm (Release, `perf_tests -tc="*20 MiB*"`, 20 MiB md, 120×40, `view_bytes = 0`, `view_lines = 0`, 3 koşu): biçimli görünüme geçiş 752 → 7–9 ms, kaydırma karesi 0,1 ms, `G` 490–502 ms (tüm görünüm), notun okunması (ham görünümle ortak) 123–129 ms. Tek satırlık 20 MiB md biçimli 285–292 ms (tek blok bölünmez). `limits.tui.view_bytes` varsayılanı (256 KiB) kalır: tek bloklu not ve `G` bütçe dışı, JSON/CSV parça parça değil (T69: 2 MiB JSON 86 ms).
- **ADDED** Ham REVIEW'da yana kaydırma: `Shift+←/→`, `Shift`+tekerlek, yatay tekerlek (bkz. "REVIEW biçimli görünüm"); uzun satırın ilk ekranından sonrası artık REVIEW'da okunur. Neden: GUI ham önizlemesiyle eşitlik.
- **MODIFIED** Notun UTF-8 denetimi kod noktası dizisi üretmez (`core::is_valid_utf8`): 20 MiB'lık notta 80 MiB'lık geçici bellek ve süresi kalktı.

## Değişiklik 2026-11-05 (NX-E)

- **FIXED** Parçalı biçimli görünümde aynı bağlantı etiketinin ilk tanımı kazanır, liste imli satırda açılan kod bloğu izlenir (bkz. SPEC-gui NX-E). Biçimli görünüme geçiş (20 MiB md, Release, 3 koşu) 16–18 → 8–9 ms: parçalara bölme ve tanım toplama tek geçişte.

## Değişiklik 2026-11-05 (NY-G bildirimler)

- **ADDED** İşlem bildirimleri (bkz. "Bildirimler"): PDF dışa aktarma, içe alma, çöp / geri getirme / silme, taşıma / kopyalama, senkron bitişi, ayar kaydı, bağlantı testi, ilişki ve harita kenarı. `tuning.notice_ms`. Neden: kullanıcı isteği, GUI ile eşitlik.

## Değişiklik 2026-11-05 (NY-T)

- **MODIFIED** Sol listenin ilk genişliği pencerenin 2/5'inden üçte birine indi ve config'e taşındı (`tuning.tui_list_share`, 0..1; geçersiz değer uyarı ve varsayılan). `<` `>` ve sürükleme ile seçilen genişlik oranı geçer; sınırlar (sol ≥ 16, sağ ≥ 24 sütun) aynı. Neden: kullanıcı isteği, iki panel ilk açılışta eşit görünüyordu.
- **FIXED** Terminalden yapıştırmada satır arasına fazladan boş satır ve sonda iki katı boş satır: CRLF iki Enter olayıydı (FTXUI `\r` ve `\n`'yi ayrı Return yapar). Bracketed paste eklendi (bkz. "Gömülü editör" Yapıştırma); desteklemeyen terminalde CR LF tek Enter. Çok satırlı yapıştırma tek satırlık girdiyi (yeni not adı, arama) ilk satırda göndermez.
- **MODIFIED** Liste harfleri CLI kısa bayraklarıyla hizalandı (kullanıcı isteği, NY-C): `n` adla ekle (eski `a`), `a` düz ekle (eski `n`), `s` ara (eski `f`; `f` gizli takma ad), `Ctrl+S` senkron (eski `s`; Önizlemede de), `S` göster (`→`/`l` takma adı), `x` PDF dışa aktar (eski seçim), `v` seçim (eski sütun), `V` sütun, `T` etiket araması (eski çöp), `t` çöp, `i` içe al (eski ayrıntı; ayrıntı Listede `Alt+→`, çöpte `i`). `E` `d` `m` aynı. GUI kısayolları (Ctrl tabanlı) değişmedi.
- **ADDED** Terminal yapıştırması `limits.tui.paste_bytes`'a uyar; bitmeyen yapıştırma `limits.tui.paste_timeout` (1 sn) sonra teslim edilir (inceleme bulgusu: kayıp `ESC[201~` tüm tuşları yutuyordu). Haritada `t` etiketlerdir, çöp `gt` (F1'de).
- **FIXED** Zaman aşımıyla biten yapıştırmanın geç gelen devamı tuş olarak çalışıyordu (Listede `d` çöpe atardı); artık metindir. Sınırı aşan yapıştırma ayrıştırıcıda tutulmaz (24 MiB yapıştırmada tepe bellek ~95 MiB büyüyordu).

## Değişiklik 2026-11-05 (NY-T3)

- **MODIFIED** Sürüklenen ya da `<` `>` ile seçilen liste genişliği artık oturumluk değil: `tui-state.toml` `list_share` (pencere oranı), sonraki açılışta `tuning.tui_list_share`'i geçer, okurken terminale sığacak şekilde sınırlanır. Neden: kullanıcı kararı (2026-11-05).
- **ADDED** Yapıştırma ayrıştırıcısının birim testleri (`tests/tui/paste_parser_test.cpp`): sınırda tam `paste_bytes`, bir bayt fazlası, sınırı ve okuma sınırını aşan bitiş işareti, zaman aşımıyla bölünen işaret. Kayıp bitiş işaretinin bedeli (bkz. "Gömülü editör" Yapıştırma) yazıldı: `paste_timeout` dolana dek basılan tuşlar metindir; zaman aşımıyla bölünen işaret metinde kalır.

## Değişiklik 2026-11-06 (NY-P önizleme doğruluğu)

- **ADDED** Markdown uygunluk koşumu (`tests/tui/markdown_conformance_test.cpp`, ctest `md_conformance`): md4c arşivindeki CommonMark spec örnekleri (652) ve GFM uzantı örnekleri (tablo 12, görev listesi 5, üstü çizili 5, izinli otomatik bağlantı 14; toplam 688) md4c'nin kendi HTML çizicisinden (`md_html`, yalnız testte bağlanır) geçer, iki arayüzün kuralıyla normal biçime izdüşürülür ve gerçek çıktıyla (GUI: `pdf::parse_markdown` blok modeli; TUI: `render_markdown` satırları ve biçim bayrakları) karşılaştırılır. Fark sayısı (GUI / TUI): önce 48 / 19, sonra 0 / 1. Kalan TUI farkı spec #40 (`&#9;foo` paragrafı baştaki boşlukla başlar). Kategori başına tavan testtedir.
- **FIXED** Adlı HTML varlıkları (`&copy;`, `&ouml;`, `&ngE;`…) çözülmüyordu: `core::decode_html_entity` md4c'nin HTML5 tablosunu kullanır (`md4c_entity`, aynı sabit arşiv); BMP dışı sayısal varlık (`&#x1F600;`) U+FFFD değil; `&#9;` sekme, `&#10;` boşluk. Bağlantı adresindeki varlık da çözülür (TUI soluk URL'si, GUI ipucu).
- **FIXED** Liste öğesindeki alıntının çubuğu satır başındaydı, metni liste girintisinin dışına kayıyordu: önek kapların sırasıyla kurulur (`MarkdownRenderer::nesting_`; çizilen son çubuğa dek gezilir, düşman iç içelik satır başına işi büyütmez).
- **FIXED** Tablo sütun hizası (`:-:`, `--:`) yok sayılıyordu: hücre sütunda ortalanır ya da sağa yaslanır (`TableData::aligns`, `CellAlign`).
- **MODIFIED** Madde imleri GUI ve PDF ile tek tablodan (`pdf::kListBullets`).

## Değişiklik 2026-11-06 (NY-P parça parça csv/json, `view_bytes = 0`)

- **MODIFIED** `limits.tui.view_bytes` varsayılanı 256 KiB → `0` (sınırsız; anahtar kalır, kullanıcı yine sınır verebilir). Neden: md, csv ve json biçimli görünümü parça parça üretilir (bkz. "Parça parça üretim"); kesim yeri olmayan tek dev blok da 128 KiB'ta zorla kesilir. Ölçüm (Release, `perf_tests`, 120×40, `view_bytes = view_lines = table_rows = 0`): 20 MiB md biçimli geçiş 11,8 ms, kaydırma karesi 0,09 ms, `G` 531 ms; tek satırlık 20 MiB md 285–292 → 5,3 ms (`G` 268 ms); 20 MiB küçültülmüş JSON 29,5 ms, kaydırma 0,09 ms; 20 MiB CSV 25,0 ms, kaydırma 0,27 ms. `G` tüm görünümü üretir: sınırsız `view_lines` ile JSON 3.451 ms, CSV 757 ms; varsayılan `view_lines` (100.000) bunu sınırlar.

## Değişiklik 2026-11-06 (NY-P `table_rows = 0`)

- **MODIFIED** `limits.tui.table_rows` varsayılanı 5.000 → `0` (sınırsız; lider kararı). Neden: csv satırları dizinli ve parça parça çizilir, md tablosu 128 KiB'ta parçalanır; tablo satırı artık belleği ya da kareyi büyütmez. Anahtar kalır: verilen sınırın üstü soluk satırla söylenir. GUI aynı anahtarı kullanır (`app::TableLimits`, `tui::ViewLimits` varsayılanları defaults.toml ile aynı).

## Değişiklik 2026-11-06 (F1 adlı ilişki, TUI)
- **MODIFIED** `L` ve harita `C` `r`: hedeften sonra ad satırı (`Tab` adlar, `Ctrl+D` yönlü, boş = adsız). Daha önce `Enter`/`r` hemen adsız ilişki kuruyordu. Neden: kullanıcı isteği "ilişki kurulurken edge'e isim verilebilsin; önceden girilmiş ilişkiler seçilebilsin" (plan-graph-map F1).
- **ADDED** Bağlantı bölümünde ve haritada ilişkiye `n` ad, `f` yön çevir, `u` yönsüz (kimlikle); ilişkili satırda yön oku ve ad; harita kapsam satırında seçili ilişkinin adı. Bildirim: kurma, ad, yön, kaldırma.

## Değişiklik 2026-11-06 (ilişki araması, TUI)
- **ADDED** `/` (ad), içerik ve `s` (genel) aramasında ad ya da içerikte bulunmayan ama etiketi, ilişki adı ya da bağlı notunun adı eşleşen
  notlar doğrudan satırların altında gelir; neden satırın sonunda soluk (`linked: b.md (uses)`, `tag: #proje`, `relation: uses → b.md`).
  Dar listede yol tam kalır, neden `…` ile kısalır; sağ sütun yalnız boş yere girer. Kural ve sıra `app::Listing`'de (SPEC-search
  "İlişki araması"), GUI ve `note find` ile aynı. Kenarlar arama başına bir kez depodan (`app::search_edges`, liste kilidiyle).
- **ADDED** `rel:` ve `linked:` argümanları (tamamlama ve boyama aynı anahtar listesinden).

## Değişiklik 2026-11-06 (F2 keşif ve düğüm işlemleri, TUI)
- **ADDED** Haritada keşif (bkz. "Harita" → "Keşif"): `x` `X` komşular, `e` `N` `#` süzgeçler, `/` haritada bul, `R` en kısa yol (`d` yönlü), `P` `U` sabitleme, `n` yeni not (panel ortasına sabit), `d` seçili notu çöpe (`u` geri). Kural `app::MapModel` (GUI ile ortak). Neden: kullanıcı isteği "map ekranından klavye ve fare ile graph db'nin (neo4j) işlevselliği" (plan-graph-map F2).
- **MODIFIED** Haritada `/` artık liste aramasını değil haritada bul'u açar; liste araması paletin `search` komutuyla açılır ve haritada eşleşenleri yine yakar. `x` `X` `e` `d` `n` haritada keşif tuşlarıdır (liste görünmez); `d` liste seçimini değil haritanın hedef notunu (işaretleri değil) çöpe atar.
- **FIXED** (inceleme) `x` merkezde bir şey eklemiyor, ikincisi haritayı tek düğüme indiriyordu: açılış görünen erimin ötesine gider, kapatma yalnız onu geri alır. Yol sınırın çizemeyeceği kadar uzunsa "sınır gizliyor" denir; yönlü yolda yalnız yolun yönündeki kenar yeşildir; adı değişen notun keşif kayıtları düşer.
- **FIXED** (inceleme) Haritada seçimsiz `d` (örneğin kenarı kaldıran `d`'den sonraki ikincisi) gösterilen notu sessizce çöpe atıyordu: artık yalnız Tab'la seçilen not atılır, yoksa ileti.
- **FIXED** (inceleme) Haritadan `n`in ad satırı Esc ile kapanınca bekleyen yer kalıyor, sonraki ilgisiz not oraya sabitleniyordu.

## Değişiklik 2026-11-06 (F3 sorgu, TUI)
- **ADDED** Haritada sorgu (bkz. "Harita" → "Sorgu"): `Q` tek satırlık Cypher (`↑` `↓` geçmiş, `Tab` görünüm adları), sonuç braille haritada vurgu, `O` liste panelinde tablo, yazmada onay penceresi (`Uygula`, varsayılan Hayır), `Esc` sonucu kaldırır. `Q` ve `O` haritada boştu (`:` palettir, `q` çıkış). Akış `app::MapQuery` (GUI ile ortak). Neden: plan-graph-map F3 (kullanıcı onayı 2026-11-05).

## Değişiklik 2026-11-06 (F4 özellik denetçisi, TUI)
- **ADDED** Denetçi: `p` (properties) REVIEW'da gösterilen notun, bağlantı bölümünün ilişkili satırında o ilişkinin, haritada `C` kipinin ilişki kenarının ya da Tab'la seçilen (yoksa gösterilen) notun özelliklerini sağ panelde bağlantı bölümünün yerinde açar: ayırıcıda konu, satırlar `anahtar = değer  (tür)`, uygulamanınki soluk, seçili ters. Tuşlar: `↑` `↓` `k` `j` `PgUp` `PgDn` gezer, `a` (add) anahtar satırı (`Özellik anahtarı (ya da #etiket):`; `#etiket` notun metnine etiket ekler) sonra değer satırı (`anahtar = (text: number: bool: ya da otomatik)`), `e` (edit) ve `Enter` seçili değeri düzenleme metniyle açar (önek türü korur), `d` (delete) onaylı siler (varsayılan Hayır; özellik ya da etiket), `Esc` `p` geri (REVIEW ya da harita), `<` `>` `z` çalışır. Salt okunur satırda `e`/`d` nedeni alt satırda söyler. Hata girdide kalır, yazılmaz. Model `app::Inspector` (GUI ile ortak). Anahtar ve değer `terminal_safe` ve hücre sınırıyla çizilir.
- **Çakışmalar:** `p` REVIEW'da ve bağlantı bölümünde boştu; haritada liste görünmez, Listenin yapıştırma `p`si haritada gölgelenir (yapıştırma liste görünümünde). Denetçi odağındayken harfler denetçinindir: `a` `e` `d` liste komutlarına gitmez (`q` çıkmaz, önce `Esc`).
- Neden: plan-graph-map F4 ("kenar ve not özellikleri ekle/düzenle/sil; ayrılmış anahtarlar reddedilir; label = etiket").
