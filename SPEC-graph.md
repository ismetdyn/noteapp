# Spec: graph

> Modül: `graph` (`noteapp_graph` kütüphanesi) · Bağımlılık: SQLite (gömülü, public domain) · Harita: [SPEC.md](SPEC.md) ·
> Durum: **ONAYLANDI** (2026-11-01, seçenek A) · Plan: [tasks/plan-graph.md](tasks/plan-graph.md) · Tasarım: [tasks/graph-design.md](tasks/graph-design.md)

## Amaç

noteapp'in tek veri kaynağı olan, sunucusuz ve gömülü bir property graph veritabanı. Notlar, meta bilgileri ve aralarındaki
ilişkiler düğüm ve kenar olarak durur; sorgu dili Cypher'ın bir alt kümesidir. SQLite yalnız kalıcılık katmanıdır: dosya biçimi,
atomik işlem ve çökme dayanıklılığını sağlar. Graph modeli, gezinme ve sorgu yürütme bu modülündür.

Küçük bir not uygulaması içindir: Kuzu'nun kolon deposu, paralel sorgu, eklenti sistemi ve milyarlarca kenar hedefi yoktur.

## Graph ilkeleri (bağlayıcı)

Bu ilkeler SQLite kullanımını sınırlar. Bir tasarım kararı bunlarla çakışırsa ilke kazanır.

1. **İlişki birinci sınıftır.** Kenar; kimliği, tipi, yönü ve özellikleri olan bir varlıktır. Bir düğümün özelliğindeki yabancı anahtar, liste veya JSON ilişki sayılmaz.
2. **Gezinme temel işlemdir.** Sorgu motoru deseni düğümden düğüme, komşuluk üzerinden yürüyerek eşler. Maliyet O(gezilen kenar × log E)'dir; tüm graf yalnız başlangıç kısıtı olmayan desende taranır. Depolama SQL'i tek tabloda nokta veya aralık okumasıdır; JOIN ve WITH RECURSIVE yasaktır, SQL metni çalışma anında üretilmez. Her iki yönde tip bazlı komşuluk kapsayan dizinlerle doğrudan okunur.
3. **Desen eşleme sorgu modelidir.** Birincil arayüz Cypher ve gezinme API'sidir. Cypher, tek bir dev SQL'e çevrilmez: plan, graph işleçlerinden oluşur (düğüm tarama, genişletme, süzme, yansıtma, sıralama). SQL yalnız depolama sınırının içinde yaşar, dışarı açılmaz.
4. **Şemasız property graph.** Etiketler, kenar tipleri ve özellik anahtarları önceden tanımlanmadan oluşur. Yeni bir ilişki türü veya düğüm türü, tablo veya migration gerektirmez.
5. **Motor nottan habersizdir.** `noteapp_graph` "not" kavramını bilmez. `Note`, `Folder`, `LINKS_TO` uygulama şemasıdır (`NoteStore` katmanı). Motor başka bir uygulamada da kullanılabilir.
6. **Kimlik kalıcı ve evrenseldir.** Düğüm ve kenar kimlikleri 128 bit, cihazlar arası benzersizdir. SQLite rowid'i veya sayaç dışarı sızmaz.

## Veri modeli

- **Değer tipleri:** `null`, `bool`, `int64`, `double` (sonlu: `inf` ve `NaN` yazılmaz, parametre ve çerçeve olarak da kabul edilmez), `string` (UTF-8), `bytes`, `timestamp`. Liste yalnız sorgu sonucunda vardır (`collect`, `labels`), saklanmaz. `bytes` bayt bayt saklanır: NUL, CRLF, BOM ve geçersiz UTF-8 korunur.
- **Depolama (iç ayrıntı, API değildir):** Kapalı tablo kümesi: düğüm, etiket, kenar, özellik, büyük değer, ops, peers, meta; etiket veya tip başına tablo ve düğümün izdüşümü olan durum tablosu yoktur. `(src, type, dst)` ve `(dst, type, src)` dizinleri paralel kenara izin verir. Büyük değerler, satır okunurken gereksiz yere yüklenmez.
- **Uygulama şeması:**
  - `(:Note {name, type, body, created_at, updated_at, origin, deleted_at, link_targets})`; `link_targets` gövdeden türer (bkz. "Sonradan oluşan hedef").
  - `(:Folder {name})` ve `(:Note)-[:IN_FOLDER]->(:Folder)`, `(:Folder)-[:CHILD_OF]->(:Folder)`. Yol saklanmaz; `IN_FOLDER` ve `CHILD_OF` gezilerek türetilir.
  - `(:Note)-[:LINKS_TO]->(:Note)`: içerikteki `[[bağlantı]]`dan üretilir, kayıtla aynı işlemde güncellenir; sonradan oluşan hedefe de o hedefin yazmasında eklenir.
  - `(:Note)-[:RELATED_TO]->(:Note)`: kullanıcının kurduğu ilişki (bkz. "Bağlantılar").
  - `(:Tag {name, display})` ve `(:Note)-[:TAGGED]->(:Tag)`: içerikteki `#etiket`ten üretilir, kayıtla aynı işlemde güncellenir (bkz. "Etiketler").
  - `(:View {name, query, created_at, updated_at})`: adı verilmiş, saklanmış salt-okunur Cypher sorgusu; kenarı yoktur (bkz. "Görünümler").
  - `origin`: `windows`, `linux`, `macos`, `android` veya geçişte `unknown`.
- **İleride:** Görsel ve çoklu ortam için `(:Blob {hash, mime, data})` ve `(:Note)-[:HAS_ATTACHMENT]->(:Blob)`. Model bunu taşır; v1'de yazılmaz.

## Depolama

- Tek dosya: `<veri dizini>/graph.db`, SQLite WAL modu, `busy_timeout`, `BEGIN IMMEDIATE`. Git'e girmez.
- Şema `user_version` ile sürümlenir, migration kodu ve testi vardır. Daha yeni sürüm dosyası açılmaz, anlaşılır hata verilir.
- CLI, TUI ve GUI aynı anda çalışabilir. Bağlantı başına tek iş parçacığı; yazma kısa işlem.

## Senkron

Git yalnız taşıyıcıdır; birleştirilen şey dosya değil, işlemdir.

- **İşlem günlüğü:** Her yazma, verisiyle **aynı SQLite işleminde** `ops` kaydı ekler. Biri olmadan diğeri kalıcı olamaz.
- **Taşıma:** Kayıtlar `<notes_dir>/graph/<cihaz-id>.oplog` dosyasına eklenir; her cihaz yalnız kendi dosyasına yazar, git düzeyinde çakışma olmaz. Bu dosyalar taşıyıcıdır: `ops` tablosundan yeniden üretilebilir ve idempotent uygulanır. Yarım çerçeve uygulanmaz, sonraki eşitlemede tamamlanır ve raporda görünür. Eşitleme önce kendi günlüğünü alır, sonra yazar. Kendi dosyası `ops` ile uyuşmazsa kenara alınır, silinmez, `ops`'tan yeniden üretilir.
- **Birleşim:** İşlemler kalıcı kimlikler üzerindedir. Çerçeveler nedensel sırayla uygulanır; sonuç eşzamanlı çerçevelerin sırasından bağımsızdır. Her yazma yalnız gördüğü sürümleri geçersiz kılar; görülmeyenler head olarak kalır. Okuma en büyük (HLC, cihaz) head'ini verir.
- **`body`:** Motor eşzamanlı head'leri `versions()`, geçmiş bir yazmayı `properties_at()` ile verir, nottan habersizdir. Uygulama önce üç yönlü birleştirir (SPEC-sync), olmazsa türetilmiş kimlikli kopya düğüm kurar.
- **Silme ve düzenleme çakışması:** Silme, düğümün tüm yazmalarını görmemişse etkisizdir: düzenleme kazanır. Eşzamanlı kenar ekleme silinmiş düğümü canlandırmaz.
- **Yol çakışması:** Deterministik `-2` eki alır.
- **Güvenilmez girdi:** Uzak günlük güvenilmezdir. Biçim, boyut, tip ve yol denetiminden geçer; işlemler Cypher veya SQL olarak yürütülmez. Sembolik bağ izlenmez, geçici dosya tahmin edilemez adla özel oluşturulur. Bozuk çerçeve uygulamayı düşürmez, bildirilir; o cihazın günlüğü ve ona bağlı çerçeveler orada bekler, diğer veri çalışır. Çerçeve içeriği ingest'i hata ile durduramaz ve yerel yazmayı engelleyemez. Bellek ve süre sınırlıdır; tanınan cihaz en fazla 256'dır. Tehdit modeli: [tasks/graph-design.md](tasks/graph-design.md).
- **Görünürlük:** `MergeReport`: `conflicts`, `bad_frames`, `stalled`, `partial_tails`, `heads` (cihaz başına günlük ve uygulanan `seq`), `omitted`. `ExportReport`: kenara alınan dosya ve nedeni.
- **Yeni cihaz:** Boş cihaz günlükleri uygulayarak grafı kurar; 5.000 notta süre sınırı aşağıdadır. Yerel `graph.db` kaybolursa aynı yolla geri gelir.

## Sınırlar (okuma kuralı)

Hepsi kümenin saf fonksiyonudur, her cihazda aynı sonucu verir; aşım raporlanır (`unreachable`), çökme ya da gizleme olmaz.

- Klasör yolu en çok 128 derinlik, 4096 bayt; altındaki notlar `invalid_folder`.
- Aynı ad en çok 1000 kopya (`-2`…`-1001`), arama ad başına doğrusal; fazlası `too_many_collisions`.
- Çakışan not başına her kopya ayrı işlem; çözülemeyen (çerçeveye sığmayan) `sync.conflict_unresolved` olur, head'ler durur, senkron sürer.
- **Kullanıcı işlemi sınıra takılmaz (T92b):** Bir yazma işleminin bütçesi `[tuning] graph_batch_ops`'tur (varsayılan 30.000, en çok 32.768: motor çerçevesinin 65.536 işleminin yarısı; uzak çerçeve bu sınırla çözülür, güvenlik sınırı olarak kalır). Bütçe bir ayardır, tavan değil: dolunca yeni iş başlamaz, kalan iş sonraki işlemlere bölünür, her işlem atomiktir. Böyle bölünür: türetilmiş kenarlar (sonradan oluşan hedefin kaynakları kullanıcının işleminden hemen sonra), çok kenarlı düğümün silinmesi (`remove`, `purge`, `empty_trash`, `remove_folder`, görünüm silme; önce türetilmiş kenarlar, klasör kenarları en son, işlem yenisinde yinelenir; klasörler en derinden), içe aktarma (`import_notes` not başına, `import_folders`), klasör kopyasının klasörleri, `relink_notes` ve ingest sonrası türetme. Türetme tamamlanmadan commit edilecek işlemden önce `tags-indexed` ve `links-indexed` işaretleri düşer: süreç arada ölürse sonraki açılış etiketleri ve eksik bağlantı kenarlarını tamamlar; yarım silinen not yerinde kalır, türetilmiş kenarları geri gelir. Yarım silmede düğümün kendi klasör kenarı son geçişe dek silinmez (not klasöründe kalır); klasöre gelen kenarlar (çöpteki notların `IN_FOLDER`ı, alt klasörlerin `CHILD_OF`u) parça parça silinir, klasör silinince çöpteki not yine atıldığı yolu bilir ve geri alma klasörü kurar; sonraki geçişler silinecek notu yoldan değil ilk geçişin kimliğinden sürdürür, başkası silmişse iş biter. Bilinen pencere (T92c): A işaretleri düşürdükten sonra B açılıp kendi türetme geçişini bitirir ve işaretleri yazarsa, A o noktadan sonra yarım bırakıp ölürse kalan iş işaretli görünür; açılış onarmaz, notun bir sonraki yazması ya da işaretin silinmesi onarır. Sınır: bütçeden çok kullanıcı kenarı (`RELATED_TO`, Cypher) olan düğümün silinmesi arada ölürse o kenarların bir kısmı silinmiş kalır.
- `graph.db`, WAL, SHM 0600, veri dizini 0700; var olanlar açılışta daralır (Windows'ta kullanıcı profili ACL'si).
- `note graph import`: satır en çok 128 MiB ve okunurken sınırlanır, satır başına 100.000 JSON değeri. Satır RFC 8259 JSON'dur (T59a): `null`, ondalık/üslü sayı, tüm kaçışlar ve surrogate çifti kabul, yalnız kalan surrogate, ham denetim karakteri ve `01` gibi geçersiz sayı ret; derinlik 6. Bilinmeyen alanda `null` taşıyan satır artık kabul edilir (onaylı yan etki); bilinen alanlarda `null` yine reddedilir.

## Bağlantılar

`NoteStore::links(path)` bir notun komşularını yollarıyla verir; ön yüzlerin bağlantı paneli yalnız bunu okur (SPEC-tui "Bağlantı bölümü", SPEC-gui).

- **Listeler:** `outgoing` (`LINKS_TO` çıkan), `incoming` (`LINKS_TO` gelen), `related` (`RELATED_TO`, iki yönde tek ilişki), `missing` (canlı nota çözülemeyen `[[hedef]]`, ham metin). Yol sıralı, tekildir: paralel kenar bir satırdır.
- **Okuma:** Komşuluk gezinmesi `neighbors` üzerindendir, kenar başına görüntüde `log N`; yol bellek görüntüsünden (`Snapshot`) çözülür. Gövde yalnız `missing` için tek kez okunur; hedefler yazmanın kuralıyla çözülür (`link_index_of`: yol, uzantısız yol, dosya adı, yalın ad; ikili not taranmaz; not başına en çok 1024 hedef, fazlası yazmada da bağlanmaz). Çöp, ulaşılamayan not ve not olmayan düğüm listeye girmez; çöpe atılan hedef `missing` olur, geri gelince kenarıyla geri döner.
- **Kenar metinden türer, yeniden adlandırma metni yazmaz:** `[[x]]` düğüm kimliğine değil metne bağlıdır. Hedef `x`, `y` olunca not gövdesi **değişmez**; kaynak bir daha kaydedilene dek kenar yerindedir, panelde `outgoing`da `y`, `missing`de `x` birlikte görünür. Kaynak kaydedilince metin artık çözülmediği için kenar düşer, yalnız `missing`de `x` kalır. Uygulama gövdeyi hiçbir koşulda kendiliğinden yeniden yazmaz: bu bilinçli sınırdır, bağlantıyı kullanıcı düzeltir.
- **Sonradan oluşan hedef (T92):** Kaynağın kaydı kenarlarını kaydın görüntüsüyle kurar. Bir not yeni bir yola geldiğinde (oluşturma, kopya, taşıma ya da yeniden adlandırma, klasör adı değişimi, çöpten geri alma, senkronla gelme) metni artık ona çözülen her notun kenarı aynı işlemde eklenir; çözüm kaydın kuralıdır (`link_index_of`, `resolve_link`) ve işlem sonrası durumla yapılır. Çöpten dönen kaynak, çöpteyken oluşan hedeflerine bağlanır. Bu ekleme yalnız ekler, kenar silmez: hedef taşınınca kenar kaynak kaydedilene dek kalır (yukarıdaki madde), daha kesin eşleşmeli yeni not eskisinin kenarını düşürmez. Kalıcı silinen hedefin kenarları düğümle gider; aynı adla yeniden oluşan not onları yeniden alır.
  - **Aday kaynaklar** gövde taranarak değil, notun `link_targets` özelliğinden bulunur: gövdedeki hedefler (`scan_links`), `\n` ile birleşik, bağlantısız notta yok. Gövdenin saf fonksiyonudur, kaydın kenarlarıyla aynı işlemde yazılır, değişmezse yazılmaz. Görüntü onu not başına bellekte tutar; yeni yolun anahtarlarını taşıyan not yoksa iş biter, varsa işlem sonrası görüntü bir kez daha kurulur. Ölçüm (Release, 5.000 not, not başına 5 bağlantı, `tests/perf/links_perf_test.cpp`, değişikliksiz kodla dönüşümlü üç tur): bağlanmamış adla oluşturma medyanı 29,2–33,2 ms'den 31,8–42,1 ms'ye (turların ortancası 32,2 → 33,6 ms; makine başka derlemeyle paylaşılıyordu); tüm notların bağladığı adla oluşturma 5.000 kenar ekleyerek 151–186 ms (test sınırı 400 ms); bağlantılı 5.000 notu alan yeni cihazın ilk ingest'i 683 ms. Seçilmeyen yol, oluşturma başına tüm gövdeleri okuyup taramak, tek başına 12,5–13,9 ms ekler.
  - **Senkron:** Alan cihaz ingest'ten sonra gövdesi değişen notlarla yeni bir yola gelen notları bağlayan notların eksik kenarlarını ekler (etiketlerle aynı geçiş). Kenar kimliği uçlardan türer ve var olan kenar yeniden yazılmaz: iki cihaz aynı kenara varır, yeniden alışveriş işlem yazmaz. Bu türetme hatası senkronu durdurmaz, loglanır.
  - **Eski veri:** `link_targets`'tan önceki grafı ilk açılış bir kez tarar (canlı notlar: `link_targets` ve eksik kenarlar), `<veri dizini>/links-indexed` işareti tekrarı önler; başarısızsa işaret yazılmaz. Her cihaz kendi geçişinde aynı değerleri yazar.
  - **Bütçe:** Kaynak sayısı sınırsızdır: kullanıcının işlemine sığmayan kaynakların kenarları commit'ten hemen sonraki işlemlerde eklenir ("Kullanıcı işlemi sınıra takılmaz"). Çakışma çözümünde kazanan gövde ve kaybedenin kopyası da kaydın kuralıyla bağlanır (`link_targets` dahil).
- **`RELATED_TO`:** `add_relation`, `remove_relation`. Yönsüz okunur; kenar kimliği uçlardan türer (iki cihazın aynı kurulumu tek kenardır), işlem olarak senkronlanır. Kaldırma iki yöndeki kenarları siler. Kendine ilişki `InvalidPath`, var olan `AlreadyExists`, olmayan `NotFound`. Not gövdesine dokunmaz, yeniden adlandırmada kalır.

## Etiketler

Etiket not metnindeki `#etiket`tir; tek kural `core::note_tags`tadır, yazma ve ön yüzler onu kullanır.

- **Sözcük:** `#`ten sonra harf, rakam, `_`, `-`, `/`; en az bir harf (`#42` etiket değildir), en çok 128 bayt, not başına en çok 256. ASCII dışı her kod noktası harftir; kontrol, Latin-1 noktalaması, genel noktalama (sıfır genişlikli ve yön karakterleri), ideografik boşluk ve BOM değildir. Sondaki `/` atılır (`#proje/` = `proje`).
- **Etiket olmayan:** `#` satır başında, boşluk, `(`, `,` ya da `;` sonrasında değilse (`a#b`, URL parçası `/sayfa#bolum`, `\#kaçış`, `&#351;`); başlık işareti (`# Başlık`, `##`); kuşatılmış kod bloğu (```` ``` ````, `~~~`; liste maddesi altındaki girintili çit de), satır içi kod, `[[bağlantı#bölüm]]` dahil `[[...]]`, Markdown çapa bağlantısı `[bölüm](#bolum)`. Yalnız düzyazı notları taranır: `md`, `markdown`, `txt` ve uzantısız; kodda `#include` etiket değildir. İkili not taranmaz.
- **Anahtar:** `tag_key`, küçük harf (search'ün `to_lower`'ı: Latin-1 ve Latin Extended-A; `İ` ve `I` → `i`). `Tag.name` anahtardır, `Tag.display` etiketin ilk yazıldığı hali. Notta aynı anahtarın ilk yazımı geçerlidir; sıra anahtarın bayt sırasıdır.
- **Yazma:** Not oluşturma, gövde yazması, birleşim ve çakışma çözümü (kazanan gövde ve kopya), tipi değiştiren taşıma `TAGGED` kenarlarını aynı işlemde günceller. Tag ve kenar kimlikleri anahtardan ve uçlardan türer: iki cihazın aynı etiketi tek düğümdür. Kenarı kalmayan Tag silinir; kullanıcının kurduğu başka bir kenarı varsa kalır. Etiketi kullanan yazma Tag düğümünü yeniden yazar: başka cihazın görmeden yaptığı sahipsiz silme etkisiz kalır.
- **Silme ve çöp:** Kalıcı silme (`remove`, `purge`, `empty_trash`, `remove_folder`) kenarları ve sahipsiz Tag'leri siler. Çöpteki not etiketini tutar ama sayılmaz, aramaya girmez; geri gelince etiketleriyle döner.
- **Okuma:** `NoteInfo::tags`/`NoteMeta::tags` anahtarlar, `NoteLinks::tags` gösterim adları, `NoteStore::tags()` canlı notu olan etiketler ve sayıları (`note tags`).
- **Eski veri:** Etiketlerden önceki grafı ilk açılış (okuma ya da yazma) bir kez etiketler, sonra `<veri dizini>/tags-indexed` işareti tekrarı önler; başarısızsa işaret yazılmaz, sonraki açılış yeniden dener. Geçiş (`relink_notes`) bağlantılarla birlikte etiketleri de kurar. Senkronla gövdesi değişen not (ingest'te revizyonu değişen, çakışmasız) alan cihazda yeniden etiketlenir: etiketsiz sürümün yazması ya da etiketlerden önceki günlük, işareti zaten yazmış yeni cihaza gelse de etiketlenir; etiketli sürümün yazmasında kenarlar doğru olduğundan işlem yazılmaz. Bu türetme hatası senkronu durdurmaz, loglanır.

## Görünümler

Görünüm adı verilmiş, saklanmış bir Cypher sorgusudur (T81). Tek kaynak `core/note_views.hpp`: `save_view`, `list_views`, `find_view`, `remove_view`, `run_view`, `view_notes`; CLI, TUI ve GUI yalnız bunları kullanır.

- **Düğüm ve senkron:** `(:View {name, query, created_at, updated_at})`, notlarla aynı işlem günlüğü ve birleşim kuralı. Kimlik katlanmış addan türer (`derive_node_id("View", tag_key(ad))`): iki cihazın aynı adı tek düğümdür, eşzamanlı düzenleme özellik başına en büyük (HLC, cihaz) head'iyle yakınsar. `name` ilk kaydın yazımıdır, üzerine yazma değiştirmez; `created_at` ilk, `updated_at` son kayıt. Kayıt düğümü her seferinde yeniden yazar: görmeden yapılmış silme etkisiz kalır ("Silme ve düzenleme çakışması"). Aynı anahtarda Cypher'la kurulmuş başka `View` düğümü de varsa türetilmiş kimlik, yoksa kimlik sırasıyla ilki okunur; adsız düğüm görünüm değildir.
- **Ad:** Klasör adı kuralı (`NotePath` bileşeni: boş, `.`, `..`, gizli, denetim karakteri, yasak karakter, sonda boşluk ya da nokta yok), ayrıca `/` yok. Eşleşme `tag_key` katlamasıyladır (`Work` = `work`). Var olan ad üzerine yazma (`overwrite`, CLI `--force`) olmadan `AlreadyExists`.
- **Salt okunur (güvenlik sınırı):** Görünüm başka cihazdan gelebilir, güvenilmezdir; çalıştırıldığında veri değiştirmemelidir. Kayıtta sorgu çalıştırılmadan ayrıştırılıp planlanır (`graph::check_query`, `ReadOnly`): yazma cümlesi (`CREATE`, `MERGE`, `SET`, `DELETE`, `DETACH DELETE`, `REMOVE`; `--write`'ın açtığı her işleç), parametre (`$x`; görünüm parametre almaz) ve bozuk sorgu `InvalidQuery` verir, hiçbir şey yazılmaz. Çalıştırma her zaman `QueryMode::ReadOnly` ve varsayılan sorgu sınırlarıyladır (süre, satır, bellek, boyut; "Sorgu dili"): kayıt denetimini atlayan yazan görünüm (senkron, `note query --write`) plan aşamasında `error.view_writes` ile reddedilir, işlem açılmaz, graf değişmez; ikinci savunma `PRAGMA query_only`.
- **Notlara çevirme (`view:`, SPEC-search):** Sonuçtaki canlı `Note` düğümü ya da canlı not yolu (`work/plan.md`) olan metin hücresi o notu seçer; çöpteki not ve not olmayan düğüm girmez. `RETURN n.name` yalnız addır, yol değildir: not için `RETURN n`. Satırı olup hiçbir hücresi düğüm ya da metin olmayan sonuç (`RETURN count(n)`) `error.view_no_notes`; satırsız sonuç boş listedir. Sonuç depo görüntüsü (`Snapshot`) başına saklanır: tuş başına süzme sorguyu yeniden çalıştırmaz, herhangi bir yazma ya da senkron görüntüyü yeniler.

## Silme

- `d`: `deleted_at` yazar, işlem olarak senkronlanır. `u` geri alır. `T` çöp kutusunu gösterir.
- `D`: Onaylı kalıcı silme: düğüm silinir ve diğer cihazlara mezar taşı (tombstone) işlemi gider.
- **Sınır:** Baytlar eski oplog satırlarında ve git geçmişinde kalır. Günlük sıkıştırma v1'de yoktur. Bu sınır arayüzde söylenir.

## Sorgu dili: Cypher alt kümesi

**Var:** `MATCH`, `OPTIONAL MATCH`, `WHERE` (`=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `NOT`, `IN`, `IS NULL`, `CONTAINS`, `STARTS WITH`, `ENDS WITH`), yön, tip ve etiket, sınırlı değişken uzunluklu yol `*1..N`, `WITH`, `RETURN`, `DISTINCT`, `AS`, `ORDER BY`, `SKIP`, `LIMIT`, `count`, `sum`, `min`, `max`, `avg`, `collect`, parametre `$x`, `CREATE`, `MERGE`, `SET`, `REMOVE`, `DELETE`, `DETACH DELETE`, `labels()`, `type()`, `id()`.

**Yok:** Saklı yordam, `LOAD CSV`, `UNWIND`, alt sorgu, `shortestPath`, kullanıcı fonksiyonu, kullanıcı tanımlı dizin, dosya ve ağ erişimi.

**Listeye eklenenler (G3):** Taşma denetimli `+ - * / %` (taşma, sıfıra bölme ve `(-9223372036854775807 - 1) / -1` hata verir), `RETURN *`, `WHERE n:Etiket`. **Bilerek yok:** `XOR`, `SET n = {…}`, `MERGE … ON CREATE`, `CASE`, liste indeksi, zincirli `a < b < c`, metin birleştirme. Hepsi konumlu `Unsupported`.

- `note query` salt-okunurdur; yazma `--write` ister. Salt-okunur modda yazma işleci plan aşamasında reddedilir, işlem açılmaz; ikinci savunma `PRAGMA query_only`.
- **Salt-okunur sınırı:** Reddedilen sorgu dosyaya hiç dokunmaz (bağlantı açıkken `graph.db` ve WAL baytları aynı). Başarılı okuma mantıksal durumu ve `ops`'u değiştirmez ama bağlantı kapanışı veya otomatik checkpoint WAL'ı `graph.db`'ye aktarabilir; `query_only` bunu durdurmaz. Yani baytlar değil, içerik sabittir.
- Çıktı: tablo veya `--json`. Derinlik, sonuç, bellek ve süre sınırlıdır; aşım kesilmiş sonuç değil `LimitExceeded`'dır. Varsayılanlar: 64 KiB sorgu, 64 derinlik ve ifade yüksekliği, 256 desen öğesi, 64 cümle, `*1..10`, 100.000 sonuç satırı, 1M gezilen kenar, 5 sn, 256 MiB tutulan satır; sorgu içi önbellek 64 MiB'de büyümeyi keser, sonuç değişmez. Desende kenar benzersizliği ve null mantığı openCypher'dır: sıralama karşılaştırmasında tip uyuşmazlığı null, `=`'de `false`.
- **Karşılaştırma:** `=`, `<`, `IN`, `CONTAINS`, `STARTS WITH`, `ENDS WITH` bayt ve UTF-8 kod noktası sırasına göredir; büyük/küçük harf ve Türkçe katlama yapılmaz (`'İ' CONTAINS 'i'` false). Katlamalı arama `search` katmanındadır. `string` ile `bytes` eşit olmaz. `int` ve `double` kesin karşılaştırılır.
- **`id()`:** 32 küçük harf onaltılık karakter; `id(n) = …` tarama değil tek okumadır.
- **`MERGE`:** Desen eşleşmezse kimlik etiket ve özelliklerden türetilir (kanonik sırada), iki cihaz aynı düğüme varır. Türetilmiş kimlik doluysa ve içerik desenle uyuşmuyorsa (ör. `SET` ile değişmiş) hata verir, üstüne yazmaz.
- Tüm uygulama yolları aynı motoru kullanır: TUI'nin listesi ve meta sorguları gezinme API'si veya Cypher'dan geçer.
- İçerik araması mevcut `ContentIndex` ve `search` katlama kuralıyla kalır; FTS5 v1'de yoktur.

## Bütünleşme

- `note query --json`, `note graph export --format jsonl`, `note graph import`.
- `note export --all <dizin>`: Notları düz dosya olarak çıkarır (`work/toplanti.md`). Veriyi uygulamadan bağımsız kurtarma yolu.
- `noteapp_graph` kamu API'si: `Graph`, işlem, düğüm/kenar okuma-yazma, `neighbors`, `query`, `export_log`, `ingest_logs`. Başlıkta SQLite türü görünmez.
- Oplog çerçeve biçimi bu dosyada ve [tasks/graph-design.md](tasks/graph-design.md) içinde belgelenir ve sürümlenir.

## Eski depodan geçiş

`note graph migrate`: Düz dosyaları içe aktarır. Önce yedekler (`<veri dizini>/migration-backup-*`: notlar, çöp, varsa `graph.db`), idempotenttir, taze bağlantıyla bayt karşılaştırmasıyla doğrular. Eski dosyalara hiçbir koşulda dokunmaz, kendiliğinden silmez.

- **Kimlik yoldan türer** (`derive_id("migrated-note", "live|trash/" + yol)`): iki cihazın aynı dosyası aynı düğümdür, aynı baytlar çakışma üretmez.
- **Değerler:** `origin=unknown`, `updated_at` dosyanın mtime'ı. `created_at` dosyanın doğum zamanı (Linux `statx` btime, Windows oluşturma zamanı, macOS `st_birthtime`), yoksa ya da mtime'dan yeniyse (kopyalanmış dosya) mtime; asla geçiş anı. Git checkout doğumu sıfırladığından klon cihazında bu checkout anıdır; iki cihaz geçirirse eşzamanlı head'lerden biri okunur. `.trash/` altı `deleted_at`'lı gelir, klasör düğümü üretmez (yol `trashed_from`'da, geri alma klasörü kurar), `.keep` ve boş dizin `Folder` olur. Gizli girdi ve bağ alınmaz, bağ raporlanır.
- **Kapı:** Geçirilmemiş eski not varsa (`<veri dizini>/migrated-notes-dirs` satırı yoksa) her yazma ve senkron `MigrationRequired` ile reddedilir, her ön yüz nedenini söyler. Okuma çalışır.
- **Geçiş yazması en düşük önceliklidir** (`WriteTxn::set_lowest_priority`, sabit HLC; motor nottan habersizdir): Kapı senkrondan önce geçişi zorladığından B, A'nın geçiş sonrası düzenlemesini görmeden içe aktarır. HLC'si geç olsa da bayat metin gerçek düzenlemeyi geçemez: gerçek yazma head'ler arasında kazanır, geçişin baytları ancak başka hiçbir yazma yoksa okunur.
- **Kopya yalnız gerçekten farklı içerik için:** Kaybeden head bir geçiş yazmasıysa (`migrated_sha` aynı dot'ta) ve aynı baytları başka cihaz da geçirdiyse o ortak tabandır, kopya üretilmez, head birleşir. Farklıysa B'nin göndermediği düzenleme kopya not olur. Düğüm zaten görülmüşse (günlükler önce alındıysa) dosya ezmez, aynı kural geçerlidir.
- **60 MiB üstü dosya** tek çerçeveye sığmaz: geçişi durdurmaz, adıyla raporlanır, yerinde kalır, kapı açılır, çıkış 0.
- **Çıkış yolu:** Diğer hatalarda (okunamayan, ad UTF-8 değil) varsayılan durup raporlamaktır. `note graph migrate --skip-failed` geçemeyen dosyaları adıyla raporlar, yerinde bırakır, kapıyı açar; hiçbir şey silmez.
- **Kapı yalnız yerel eski notta kapanır:** `enable` klonu ya da eski not görülmeden başlayan senkron `migrated-notes-dirs`'e işler: git'le sonradan gelen düz dosya kilitlemez, not sayılmaz.
- **Dışarıda kalanlar kalıcıdır (2026-11-02):** `skipped` ve `failed` dosyaları tam yol ve nedenle `<veri dizini>/migration-skipped`e yazılır (`neden<TAB>yol`, özel bayt `%XX`); geçiş yinelenince o `notes_dir`in kayıtları yenilenir, başka dizininki kalır. `note graph status` geçerli `notes_dir`in, `note export --all` tüm listenin kayıtlarını yazar, 1 ile çıkar; `--ignore-skipped` yazar, 0 ile çıkar. Status başka dizinin kayıtlarını yalnız sayar. `note graph status --clear-skipped` tüm listeyi yazıp unutur, eski dosyalara dokunmaz. Bozuk liste 3 ile çıkar, "atlanan yok" sayılmaz.
- **Geçişten sonra düşen dosya görünür (T88, 2026-11-02):** Geçişi bitmiş kökte `note graph status`, geçişin gezdiği ağaçta (canlı + `.trash`; gizli, `graph/` taşıyıcısı ve bağ hariç) `verify_imported` ile uyuşmayan, aynı yoldaki canlı notla da bayt bayt aynı olmayan düz dosyaları `new_file` (içe alınamayacaksa katalog nedeniyle) listeler, 1 ile çıkar; `migration-skipped` kayıtları yinelenmez, `--ignore-skipped` bu listeyi etkilemez. Sınır: 100.000 girdi, 1 GiB okuma; aşılırsa "kalan yok" denmez, 1 ile çıkar. Çıktıda ilk 200 dosya. `note graph migrate --new` yalnız bunları `import_notes`'la içe alır: kimlik yine yoldan türer, farklı bayt kopya not olur; aynı adlı uygulama notu `assign_unique_paths` ile ayrışır. Yedek yalnız yeni dosyalar ve `graph.db`dir, yeni dosya yoksa alınmaz; klasör kurmaz. Silme (`remove`, `purge`) düğümün gövdesini ve `migrated_sha`'sını da sildiğinden içe alınan baytlar grafın dışında da tutulur: `<veri dizini>/migrated-digests` (`sha256-hex<TAB>tam yol`, `%XX`, 0600, atomik, yalnız eklenir, bütün kökler tek dosyada). Bu yolda bu baytlar kayıtlıysa dosya yeni değildir; silinen not geri gelmez, sonradan değişen dosya yine listelenir. Geçiş ve `--new` doğruladıklarını, `--new` grafta bulduklarını da yazar: kayıttan önce geçirilmiş kökler, notları hâlâ duruyorsa böyle dolar; önceden silinmiş notunki dolmaz (yerel kayıttır, senkronlanmaz). Her açılışta CLI taraması yok; TUI açılışta işçide 2000 girdi, 8 MiB sınırıyla tarar (ölçüm: Debug, 4 KiB'lik 2000 dosya, `ls` 108 ms, `status` 717 ms, yani ~600 ms; UI'da yapılamaz), bulursa ilk tuşa dek sönük ipucu, bulamazsa ya da sınırda sessiz. `migrated-notes-dirs` 0600; eski 0644 dosya bir sonraki işaretlemede daralır.
- **Okunamayan girdi sessiz kalmaz (2026-11-02):** Listelenemeyen dizin (izin, Windows'ta MAX_PATH) içi bilinmese de `unreadable_folder`, durumu okunamayan girdi `unreadable` ile `failed`'a girer; `.trash` altındakiler gerçek yoluyla. Böyle ağaç "boş" sayılmaz: kapı kapalı kalır. Yalnız kökün kendisinin yokluğu hata değildir.
- **Yedek özeldir:** Dizin 0700, dosyalar 0600; yer ve silme komutu `note graph migrate` çıktısındadır.
- **Sınır:** `NotePath`'ten geçmeyen ad eski listede de görünmüyordu, atlanır ve raporlanır.

`note export --all <dizin>`: Baytlar aynen, boş klasörler dahil; çöp `<dizin>/.trash/` altına (`NotePath` noktayı reddettiğinden canlı notla çakışmaz). Yol `NotePath`'ten yeniden geçer, hiçbir bileşen bağ olamaz, var olan dosyanın üzerine yazılmaz. Reddedilen ve **ulaşılamayan** not (ad ya da klasör geçersiz, çok derin, ad çakışması sınırda) kimliğiyle raporlanır, çıkış 1: eski dosyaları silmeden önceki denetim budur. Ulaşılamayan notun baytları `note graph export`'tadır.

`note graph export --format jsonl` ve `note graph import`: Motor düzeyinde, nottan habersiz: düğüm, etiket, kenar, özellik, kimlikler korunur. Eşzamanlı head'lerden yalnız okunan değer yazılır: taşıma biçimidir, senkronun yerine geçmez. Tekrar içe aktarma işlem yazmaz. Üçü de günlüğü sıkıştırmaz.

### Var olan senkron deposunda geçiş (iki cihaz)

1. Cihazlarda eski sürümle son kez `note sync` (isteğe bağlı ama gürültüyü azaltır).
2. A: yeni sürüm, `note graph migrate`, `note sync`. Eski dosyalar depoda kalır, senkron onlara dokunmaz.
3. B: yeni sürüm, `note sync` reddedilir ve yönlendirir, `note graph migrate`, `note sync`. Sıra önemli değildir; aynı baytlar tek not olur.
4. Her iki cihazda `note graph status` ve `note export --all DİZİN` ile denetle; dışarıda kalan dosya varsa ikisi de 1 ile çıkar, o dosyaları silme. Sonra tek cihazda: `git -C <notes_dir> rm -r -- <eski dosyalar>`, `git commit`, `note sync`.

## Kabul kriterleri

1. **Graph ilkeleri:** Kenar okuma, grafın boyutundan bağımsızdır: 5.000 ve 50.000 düğümde sabit komşulukla iki atlamalı başlangıç sorgusu medyan süre oranı ≤ 1,5 kalır. Yeni etiket veya kenar tipi şema değişikliği gerektirmez. Başlıklarda SQLite ve not kavramı yoktur.
2. **Kalıcılık:** `kill -9` sonrası commit edilen işlem ve `ops` kaydı birlikte görünür; yarım işlemden parça kalmaz.
3. **Byte koruma:** CRLF, BOM, NUL, geçersiz UTF-8 ve 10 MiB `bytes` birebir geri okunur.
4. **Yakınsama:** Rastgele işlem dizileri, rastgele sıra ve tekrar uygulamada iki, üç ve dört cihaz aynı graph'a varır. `body` hiçbir eşzamanlı senaryoda kaybolmaz. Log ile dosya arasındaki her çökme noktası test edilir.
5. **Bozuk girdi:** Bozuk oplog ve hostile Cypher fuzz'unda (ASan/UBSan) crash, hang ve bellek hatası olmaz; ayrı olarak en az 2 milyon girdi çalıştırılır.
6. **Sorgu:** Desteklenen her öğe için doğru sonuç; desteklenmeyen sözdizimi konumlu, anlaşılır hata verir. Yanlış `ok` dönmez: salt-okunur modda her yazma denemesi hata verir ve dosyaya yazmaz. `LOAD CSV`, dosya ve ağ erişimi yoktur.
7. **Davranış koruma:** Mevcut CLI, TUI ve GUI testleri, depo değişikliğinden etkilenen yol ve zaman ayrıntıları dışında aynen geçer.
8. **Performans (Release, 5.000 not, 2 KB ortalama):** `note "x"` toplam ≤ 200 ms; açılış ≤ 300 ms; tuş başına liste ≤ 50 ms; 5.000 başlangıç, ortalama 4 çıkış kenarı, `ORDER BY` + `LIMIT 20` ile iki atlamalı sorgu ≤ 50 ms; 5.000 notluk yeni cihaz kurulumu ≤ 10 sn; `sqlite_stat1` varken ve yokken aynı sorgu planı.
9. **Taşınabilirlik:** Linux ve MinGW çapraz derlemede statik; tek `.exe`, yeni DLL yok. SQLite pinli ve lisans notu dağıtımda.
10. **Geçiş:** Her örnek depo geçişten sonra bayt bayt aynı notları verir; `note export --all` çıktısı özgün dosyalarla aynıdır.
11. **Bağlantılar (Release, 5.000 not, 20.000 kenar, `tests/perf/links_perf_test.cpp`):** `links` olağan notta medyan ≤ 5 ms, 4.999 geleni olan notta ≤ 25 ms.

## Kapsam dışı

Ağ sunucusu, çoklu kullanıcı, paralel sorgu, kolon deposu, vektör ve embedding araması, tam Cypher uyumluluğu, Neo4j protokolü, otomatik git geçmişi temizliği.

## Kararlar

- **GraphQLite reddedildi (2026-11-01):** Denemede NUL içeren değeri sessizce kesti, fuzz'da 9 benzersiz crash verdi, `LOAD CSV` salt-okunur bağlantıda dosya okudu ve iç kimlikleri cihazlar arası senkronu bozdu. Kanıt: `worktree-agent-a2b40f8ab48086ccf` dalı, `spike/graphqlite/`.
- **LadybugDB reddedildi:** MinGW desteği görünmüyor, boyut ve kapsam bu iş için fazla.
- **SQLite tercih edildi:** Denemede raw SQLite iki atlamalı sorguda 10,7 ms, listelemede 1,3 ms verdi; `kill -9` turlarında atomiklik korundu; UUID'li işlem günlüğü 4 kopyada yakınsadı.
- **Tek kaynak:** Düz dosya ve yan dosya modeli reddedildi. SPEC.md §1 "düz dosya" ilkesi bu modülle değişir; düz dosya yalnız dışa aktarma biçimidir. `graph.db` asıl veridir; oplog dosyaları ondan üretilen taşıyıcıdır.
