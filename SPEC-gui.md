# Spec: gui

> Modül: `gui` (`notes-gui` çalıştırılabiri, SDL3 + Dear ImGui) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Aynı notları masaüstü penceresinde oluşturmak, düzenlemek, klasörlerle düzenlemek ve klavyeyle hızlıca
gezinmek; ayarları bir sekmeden yönetmek; PDF dışa aktarmak.

## Yerleşim

Tek SDL3 penceresi, Dear ImGui sekmeleri: **Notlar** | **Ayarlar**. Durum çubuğu: senkron durumu, not sayısı.
Mimari: `GuiState` (saf model, ImGui/SDL bilmez; testlerin çoğu burada) + `View` (her karede `GuiState`'i çizer, kullanıcı
niyetini kare bittikten sonra ona iletir). Olay döngüsü `SDL_WaitEventTimeout`; boştayken sürekli çizilmez. AutoSync
sonucu işçiden ana iş parçacığına `GuiState::post` kuyruğuyla geçer, `SDL_PushEvent` yalnız uyandırır.

**Ölçek:** arayüz ölçeği pencereninkidir: `SDL_GetWindowDisplayScale / SDL_GetWindowPixelDensity`. ImGui pencere koordinatında
çizer ve `DisplayFramebufferScale` pikseli zaten karşılar; bölme yapılmasa Wayland 2x'te (ölçek 2, yoğunluk 2) arayüz 4 kat çizilirdi.
X11/Windows'ta yoğunluk 1'dir. `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` ölçeği stil, yazı, en küçük pencere ve bölmelerle yeniden uygular.
**Sığdırma:** en küçük pencere 640x420 x ölçek (SDL alt sınırı; daha küçük kayıtlı boyut büyütülür). Sığmayan tek satırlık metin kısalır, tam metin
ipucundadır: başlık yolu sondan korunur (dosya adı ve kaydedildi/değişti görünür), durum çubuğu ve liste/klasör satırları baştan; durum çubuğunda
satır sonları boşluğa iner. Durum iletisi ve senkron metni ikisi de uzunsa alanı yarı yarıya paylaşır. Düğme sıraları sığmayınca alta iner. Diyalog pencere içinde kalır: genişliği ve yüksekliği pencereyle sınırlıdır, taşan içerik kaydırılır, taşınmış diyalog
pencere küçülünce içeri alınır. Bileşikçi alt sınırı uygulamasa (1x'ten 2x'e taşıma) da genişlikler negatife düşmez: diyalog yazısı ve form alanı en az 4 em kalır.
**Bölme boyutu:** `gui-state.toml`'a yalnız kullanıcı (sürükleme, kenara çift tıklama) yazar. Pencere küçülmesi ya da araç çubuğunun alta inmesi bölmeyi yalnız ekranda kırpar,
yer açılınca kayıtlı boyuta dönülür.

**Notlar sekmesi** (yatay splitter):
- **Araç çubuğu:** iki bölmenin üstünde, pencere genişliğinde tek çubuk. Düğmeler (simge `icons` ayarına bağlı) mevcut eylemleri çağırır ve **seçili satıra** uygulanır; gruplar ayraçla ayrılır: Yeni not, Yeni klasör | Yeniden adlandır, Taşı, Sil | Çöp (n), PDF'e aktar; sağa yaslı görünüm seçici Ağaç / Ayrıntılar / Harita.
  Yeni not ve Yeni klasör hedefi `GuiState::target_folder()`: seçili klasör, yoksa gösterilen notun klasörü, yoksa kapsam klasörü. Yeniden adlandır klasör satırında klasörü, not satırında notu işler (kök klasör hariç). Sil not satırında onaysız çöpe atar (durum çubuğunda ileti ve Geri al), klasör satırında klasörü çöpe atar (notlarıyla; notlu klasörde sayıyla onay, notsuz klasörde onaysız; Geri al hepsini getirir), çöp görünümünde onaylı kalıcı siler. Çöp (n) liste panelinde çöp görünümünü açar. İpucunda kısayol; kapalı düğmede de ipucu görünür.
  Kapalı olma: Yeniden adlandır ve Sil kök klasör satırında ve seçim yokken (çöp görünümünde Yeniden adlandır hep; Sil çöp satırı yokken); Taşı gösterilen kayıtlı not yoksa; PDF ikili notta, boş taslakta (`Ctrl+E` onu atardı) ve not yokken. Dar pencerede düğmeler alt satıra iner (görünüm seçici sağda kalır). Bağlam menüleri ve kısayollar kalır.
- Sol: arama kutusu + mod düğmesi (Ad/İçerik/Tümü) + Sırala düğmesi → liste paneli. Arama ve çöp kapalıyken panel **gezgin**dir, `BrowserView` üç görünümden birini gösterir (seçim `gui-state.toml` `view_mode` anahtarında, `tree`/`details`/`map`; bilinmeyen değer `tree` olur). Harita aramada da kalır (eşleşmeleri vurgular), çöp açılınca çöp listesi görünür:
  - **Ağaç** (varsayılan): klasörler ve notlar tek açılır ağaçta, kök "Tümü"; her düzeyde önce klasörler, sonra notlar, kullanıcının sırasıyla (aşağıda **Sıralama**). Açık klasörler `gui-state.toml` `open_folders` anahtarında (TUI'nin `tui-state.toml` anahtarıyla aynı ad) kapanışta yazılır; ağaçta artık olmayan klasör yazılmaz.
  - **Sıra no (T82):** Ayarlar → Görünüm → `Sıra no` açıkken (`gui-state.toml` `row_numbers`, varsayılan kapalı) Ağaç'ta girintiden sonra, Ayrıntılar'da ilk `#` sütununda soluk, sağa yaslı numara. Numara TUI kuralıyla klasörler ve notlar birlikte, her klasörde 1'den (`BrowserRow::number`); kökün numarası yok. Sütun genişliği en büyük numaradan. Arama ve çöp listesi numarasızdır.
  - **Ayrıntılar:** yalnız geçerli klasörün doğrudan çocukları, aynı sırayla, tabloda: Ad, Boyut, Değiştirilme, Oluşturulma. Üstte Yukarı düğmesi (kökte kapalı) ve her ata için tıklanabilir yol düğmesi (kök "Tümü"). Klasöre girmek `GuiState::enter_folder` ile olur (README: çift tıklama ya da `Enter`). Tablo dar panelde yatay kaydırılır.
  - **Harita (T84):** aşağıda "Harita".
  - Arama yazılınca ya da çöp açılınca görünüm düz listeye döner (Harita'da yalnız çöp): göreli yollar, eşleşen karakterler renkli vurgulu — kalın değil; içerik modunda eşleşme sayısı. Liste boşsa not oluşturmayı anlatan tek ileti.
  - **Sorgu argümanları (T80):** TUI/CLI ile aynı ayrıştırıcı (`search::parse_query`, SPEC-search): `tag:proje`, `type:md`, `in:work/`, `size:`, `updated:`, `created:`, `origin:`, `sort:`, kayıtlı görünüm `view:AD` (T81; yalnız arama kutusundan, ayrı arayüz T82). Ad modunda argümanlar süzer, kalan metin fuzzy sıralar (metin yoksa en yeni önce); içerik modunda argümanlar süzer, kalan metin içerikte aranır, editör seçimi de o metnin ilk geçişidir. `sort:` verilirse o sıra. Bozuk değer metin sayılmaz: kutunun altında TUI'nin `format_error` metni kırmızı, liste son geçerli haliyle kalır. `sync:` için not başına senkron durumu `app::SyncStatusWorker` (TUI ile aynı işçi) ile UI iş parçacığı dışında doldurulur: senkron açıkken liste değişince, senkron turu bitince ve ayar değişince; sonuç gelene dek notlar `sync_off` görünür.
  - **Genel arama (R3):** `Ctrl+Shift+F` (ya da mod düğmesi `Tümü`'deyken) ad ve içerik birlikte (`search::search_all`, TUI `f` ile aynı); içerikte eşleşen satır sayısı yolun yanında, en iyi satır yolun altında soluk ve eşleşmesi vurgulu (bu modda her satır iki satır boyundadır). Eşleşmesiz genel aramada `Enter` not açmaz. Mod düğmesi ve `Ctrl+Shift+M` addan içeriğe, içerikten ve genelden ada çevirir (TUI kuralı).
  - **Girdi satırı (G4):** arama kutusu TUI'nin girdi satırıdır (`tui::InputLine`, SPEC-tui "Girdi satırı"; metni ve imleci ImGui tutar, model her karede eşitlenir). `Tab` tamamlar: tek aday doğrudan yazılır, çoksa kutunun altında liste (odak kutuda kalır; yazı listeyi süzer, `↑↓` gezer, `Enter` ekler, `Esc` ya da odağın gitmesi kapatır, fare tıklaması ekler); adaylar argüman anahtarları, `tag:`/`view:`/`type:`/`in:` değerleri, not adları ve klasörler. Gölge öneri (soluk) imleç sondayken: önce geçmiş, sonra not adı; `→` alır. Argüman anahtarı camgöbeği, değeri yeşil, bozuk değer (`parse_query`'nin aralığı) kırmızı; kutu metni saydam çizer, renkli metni üstüne `View::draw_search_text` çizer. **Geçmiş:** `Enter`le kullanılan, eşleşen ve bozuk olmayan arama `gui-state.toml` `search_history`'e girer (TUI `tui-state.toml` ile aynı ad ve kural: en yeni başta, yinelenmez, `limits.tui.search_history` / `search_entry_bytes`, sır benzeri ve not adı taşıyan sorgu yazılmaz, okurken de süzülür). `Ctrl+P` geçmişte geri gider (şu anki metni içeren), `Ctrl+N` yalnız geçmişte gezerken ileri gider ve yazılan metne döner; gezmiyorken kutuda da yeni nottur. `↑↓` listede gezmeye devam eder (TUI gibi).
  - **Etiket ve görünüm seçici (R3):** `Ctrl+Shift+T` etiketleri (`#ad  (sayı)`), `Ctrl+Shift+R` kayıtlı görünümleri arama kutusunun altında açılır listede gösterir; seçilen değer ad modunda `tag:DEĞER` ya da `view:DEĞER` sorgusu olur (boşluklu değer tırnaklı). Dışarı tıklama listeyi kapatır.
  - **Sıralama (R3, karar 6):** Sırala düğmesi TUI `o` menüsünün komutlarını (`sort.name`, `sort.updated`, `sort.created`, `sort.size`, `sort.reverse`) listeler; işaret geçerli alanda ve sıra ters çevrilmişse `Sırayı ters çevir`de. Ad artan, öbürleri en büyük/en yeni başta seçilir (`app::sort_for`). Sıra ağaca, Ayrıntılar'a ve metinsiz aramaya uygulanır; sorgudaki `sort:` onu ezer. Varsayılan TUI ile aynı: en yeni başta. Seçim hemen `gui-state.toml` `sort` / `sort_descending` anahtarlarına yazılır (TUI adlarıyla); bilinmeyen değer varsayılana düşer.
  - Klasör satırı seçilince açık notun bekleyen düzenlemesi önce kaydedilir; sağ panel editör yerine klasör özetini gösterir (yol, alt ağaçtaki not ve klasör sayısı, ipucu).
- **Çöp görünümü (`Çöp (n)`):** liste paneli çöpü gösterir, üstünde başlık ve Geri getir, Kalıcı sil, Boşalt (ikisi onaylı, varsayılan Vazgeç), Geri düğmeleri. `↑↓` çöp satırlarını gezer; `Del` ve `Shift+Del` onaylı kalıcı siler. Arama yazmak, klasör seçmek ya da yeni not çöpten çıkarır. Sağ panel açık notta kalır; çöp önizlemesi ve satır ayrıntısı GUI kapsamı dışında.
- Sağ: başlık şeridi (yol, kaydedildi/değişti göstergesi, notun `#etiket`leri, `md`/`markdown`/`csv`/`json` notunda **Önizleme/Düzenle** düğmesi, **tip açılır kutusu**) → `InputTextMultiline` (gömülü DejaVu Sans Mono) ya da önizleme → bilgi paneli → bağlantı paneli. Etiketler (T80) `NoteLinks::tags`tan, bağlantılarla aynı okumada gelir: kayıtta ve yeniden taramada tazelenir, yazarken değil. **Gizli karakterler (T91):** tüm GUI fontları `core::hidden_controls` kod noktalarını (SPEC-tui "Karakter": bidi denetimleri, sıfır genişlikliler, U+2061–U+2064, Hangul dolguları, U+180E; ZWNJ/ZWJ hariç) `U+FFFD` glifine yönlendirir (`ImFont::AddRemapCodepoint`): editörde, önizlemede ve adlarda yerinde görünürler, InputText imleç ölçüsü tutarlı kalır, baytlar değişmez. Tampon böyle karakter taşıyorsa başlıkta kırmızı uyarı `3 gizli bidi/sıfır genişlikli karakter` durur (yol kısalır, uyarı kısalmaz; ipucu nedenini söyler); sayı içerik değişince yeniden sayılır.
- **Bağlantı paneli:** editörün altında katlanır başlık `Bağlantılar (5)`; içinde `Gelen`, `Giden`, `İlişkili`, `Eksik` grupları (`NoteStore::links`, SPEC-graph "Bağlantılar"). Bağlantısız notta panel yoktur, editör tam boydur. Yükseklik en çok 8 satır ve sağ alanın %40'ı, kalanı kaydırılır, binlerce satırda `ImGuiListClipper` yalnız görüneni çizer. Satıra tıklamak o notu açar: tampon kaydedilir, hedefi gizleyen süzgeç gevşer (`GuiState::open_link`). Eksik `[[ad]]` soluk ve tıklanmaz; adı değişen hedef burada görünür, metin yeniden yazılmaz. Ad güvenilmezdir: `##` ve `%` yorumlanmaz, denetim baytı `U+FFFD` olur. Katlanma oturumluktur. Veri not gösterilince, kayıtta ve yeniden taramada okunur. İlişki kurma GUI'de yok: `L` TUI'dedir (SPEC-tui "Bağlantı bölümü").
- **Bilgi paneli (T82):** editörün altında, bağlantı panelinin üstünde katlanır başlık `Bilgi` (`Ctrl+I`); TUI'nin satır ayrıntısıyla aynı alanlar (boyut, güncelleme, oluşturma, senkron, kaynak cihaz) ve tip, etiketler. `created` ve `origin` boşken "bilinmiyor", etiket ve tip yoksa "yok". Senkron durumu `sync::fill_sync_status` ile yalnız gösterilen not için doldurulur (senkron kapalıyken `senkron kapalı`). Değerler bağlantılarla aynı anlarda tazelenir (gösterim, kayıt, yeniden tarama); boyut ve zaman listedeki kayıttan gelir. Sığarsa iki alan çifti yan yana (yedi alan dört satır), yoksa tek sütun; yükseklik bağlantı paneliyle aynı kuralla sınırlı (en çok 8 satır, kalan alanın %40'ı), kalanı kaydırılır. Kapalı başlar, katlanma oturumluktur. Taslakta ve klasör seçiliyken panel yoktur. Kaynak cihaz adı güvenilmez: denetim baytı `U+FFFD` olur.
- **Önizleme (T82, G4):** yalnız gösterilen kayıtlı `md`, `markdown`, `csv` ya da `json` notunda (TUI REVIEW ile aynı kural, tek sahibi `app::preview_kind_for`), düzenlenebilir metinde (geçerli UTF-8, NUL yok) ve `limits.tui.view_bytes` (256 KiB; TUI biçimli görünümüyle aynı anahtar, `0` sınırsız) altında; aksi hâlde düğme kapalıdır, nedeni ipucunda. `Ctrl+Shift+V` ya da başlıktaki düğme önizleme ↔ düzenleme; seçim oturum boyunca tüm önizlenen notlarda geçerlidir (TUI `R`), önizlenemeyen notta editör görünür. Düzenlemeye dönüş odağı editöre verir; editör aynı ImGui kimliğiyle çizilir, kaydırma ve imleç ImGui durumundan gelir (gözle doğrulanmadı); önizleme konumu kaynağa eşlenmez. Önizleme salt okunurdur ve tamponu (kaydedilmemiş düzenleme dahil) gösterir.
  - **Ayrıştırma:** PDF dışa aktarmanın md4c yürüyüşü (`pdf::parse_markdown`, `MarkdownOptions::links`): aynı bayraklar ve `MD_FLAG_NOHTML`; önizleme ayrıca bağlantı hedeflerini ve `[[wiki]]` bağlantılarını (`MD_FLAG_WIKILINKS`) ister, PDF istemez (çıktısı değişmez). TUI'nin `render_markdown`'ı terminal hücresine sarılmış satır ürettiği için kullanılmaz.
  - **Çizim** (`src/gui/markdown_preview.*`): başlıklar gömülü DejaVu Sans Bold ile 1,6 / 1,4 / 1,2 / 1,1 em; vurgu Oblique / Bold / BoldOblique; listeler asılı imle (`•`, `1.`, `[x]`) derinlik başına girintili; kod bloğu DejaVu Sans Mono, zeminli, boşluklar korunur, sarılmaz; satır içi kod `ImGuiCol_TableBorderLight` zeminli (açık ve koyu temada pencere zeminine karşı ≥ 1,5, metne karşı ≥ 4,5 karşıtlık); alıntı solda çubuk; yatay çizgi; tablo tek satırlık hücreler, başlık kalın, ızgara çizgili. Metin boşlukta sarılır, satırdan geniş sözcük bölünür; kod ve geniş tablo yatay kaydırılır. Görsel yerine alt metni (yoksa `pdf.image_placeholder`), HTML düz metin. Ayrıştırma metin değişince, yerleşim genişlik ya da yazı boyu değişince yeniden kurulur; karede yalnız görünen koşular çizilir.
  - **Bağlantılar:** `[[hedef]]` metni (takma adda takma ad) bağlantı renginde ve altı çizili. Tıklama `GuiState::open_wiki_link`: bekleyen düzenleme önce kendi notuna yazılır (`flush_for`), hedef sonra `NoteStore::resolve_link` ile yazmanın kuralıyla (yol, uzantısız yol, dosya adı, yalın ad; `|takma ad` ve `#başlık` atılır) çözülür ve `open_link` ile açılır; çözülmezse durum çubuğunda `bulunamadı` iletisi, not değişmez. Çözülmeyen hedef bağlantı panelindeki `Eksik` gibi soluk ve üstü çizilidir, tıklanmaz (`GuiState::resolves_link`); hedef sonradan oluşursa (kayıt, senkron) yeniden taramada eksik listesi değişir, yerleşim yeniden kurulur ve bağlantı canlanır. Dış bağlantı (`[metin](url)`) açılmaz: adres yalnız ipucunda gösterilir (not güvenilmez).
  - **CSV ve JSON (G4, matris 55–57):** ayrıştırma TUI REVIEW'unkiyle aynıdır (`app::parse_csv`, `app::pretty_json`; TUI yalnız terminal çizimini tutar). CSV: ayırıcı `detect` kuralıyla, ilk satır başlık zeminli, ImGui tablosu (`ImGuiListClipper`); en çok `limits.tui.table_rows` satır ve `table_columns` sütun (ImGui tablosunun 512 sütun tavanı da), kesilen satır ve sütun soluk satırla söylenir. JSON: iç içelik başına iki karakter girinti (genişliğin yarısını geçmez), belirteçler editörün sözdizimi kuralıyla renkli (`tui::highlight_line`, `limits.tui.highlight_line_bytes`), en çok `limits.tui.view_lines` satır, fazlası soluk satırla. Tablo olmayan CSV ve geçersiz JSON nedenle birlikte ham metin gösterir. Hücre ve dizgi metni güvenilmezdir: denetim baytı `U+FFFD`, `##`/`%` yorumlanmaz.

## Harita

Görünüm seçicideki **Harita** sol bölmede notları çizge olarak gösterir; editör sağda kalır. Sol bölmenin genişliği Harita'da ayrıdır
(`gui-state.toml` `map_pane_width`, ölçek 1 biriminde, varsayılan 600): harita dar ağaç bölmesine sığmaz, geniş harita ağacı genişletmez.
- **Çizge:** düğüm canlı nottur (isteğe bağlı etiket de); kenar `LINKS_TO` (oklu), `RELATED_TO` (oksuz, mor), etiket açıkken `TAGGED` (soluk).
  Eksik hedef, çöp ve ulaşılamayan not çizilmez. Veri `NoteStore::edges()`: tüm canlı notlar arası `LINKS_TO` ve `RELATED_TO`, tek okuma işleminde,
  yönsüz ilişki tek satır. `links()`'ten farkı: metinde olup kenarı kurulmamış hedef (kaynaktan sonra oluşmuş, SPEC-graph "Sonradan oluşan hedef")
  yoktur; böyle bağlantı kaynak yeniden kaydedilene dek haritada çizilmez, bağlantı panelinde görünür (gövde not başına okunmaz).
- **Okuma:** kenarlar yalnız taramada değişiklik görülünce okunur (listeleme değişti ya da `NoteStore::data_version`: başka bağlantının yalnız ilişki
  değiştiren yazması); harita görünür değilken hiç okunmaz. Kare başına depo çağrısı yoktur. Görünür harita seçenek ya da çevre merkezi değişince
  bellekten kurulur (`build_note_map`, `src/gui/note_map.*`, saf).
- **Kapsam:** `Tüm çizge` kapalıyken (varsayılan) gösterilen notun çevresi: bağlantı ve ilişki kenarlarıyla yönsüz en çok `Derinlik` adım (1–6, varsayılan 2).
  Gösterilen not yoksa bütün çizge. `Etiketler` gösterilen notların etiketlerini küçük kare düğüm olarak ekler; etiket üzerinden gezilmez. Seçenekler oturumluktur.
- **Sınır:** en çok `kMaxMapNodes` = 2.000 düğüm çizilir (notlar önce: çevrede yakın, bütün çizgede en yeni; etiket kalan yere). Aşılınca denetimlerin
  altında `{total} düğümden {shown} tanesi gösteriliyor` uyarısı çıkar.
- **Yerleşim** (`src/gui/graph_layout.*`, saf): Fruchterman–Reingold'un ızgara sürümü (itme yalnız 2k yarıçapında) ve zayıf merkez çekimi. Başlangıç konumu
  anahtarın FNV-1a özetinden (tohum sabit, sıra bağımsız); yeni not eski komşusunun yanında doğar. Kare başına `View::kMapIterationsPerFrame` (1) adım,
  sıcaklık her adımda soğur, eşiğin altında durur: boşta kare üretilmez. Yeniden taramada duran notun konumu ve iğnesi korunur; çizge aynıysa yerleşim uyanmaz.
- **Çizim:** düğüm rengi tip renginden (`core::file_icon`, Ağaç'taki ikon rengi), gösterilen not halkalı; komşular yeterince açıkken tüm adlar, değilse yalnız
  gösterilen ve üstündeki notun adı. Ad kısalır, tam yol ipucundadır, başka adın üstüne binen ad çizilmez, tuval kenarında içeri kaydırılır. Ad güvenilmezdir:
  biçim dizgisine girmez, denetim baytı `U+FFFD` olur. Kenarlar ImGui üçgeni değil `SDL_RenderLine` ile çizilir (çizim listesinde geri çağrı): SDL'nin yazılımsal
  üçgen doldurucusu sınır kutusundaki her pikseli sınar ve dokulu dolguda piksel başına bölme yapar; 5.000 uzun kenar kareyi 40 ms'ye çıkarıyordu.
- **Etkileşim:** tıklama notu açar (`GuiState::open_map_note`: bekleyen düzenleme önce kendi notuna yazılır, `flush_for`; sonra `open_link`), çift tıklama açıp
  Ağaç'a geçer (kayıttan vazgeçilirse görünüm kalır), düğümü sürüklemek onu bırakıldığı yere sabitler. Tekerlek imlecin çevresinde yakınlaştırır; boşluğu sol
  tuşla ya da orta tuşla sürüklemek kaydırır. Tuval odaktayken oklar kaydırır, `+`/`-` yakınlaştırır (SDL tuş kodundan: Türkçe Q'da `+` Shift+4'tür), `0`
  sığdırır. Kamera kendiliğinden sığdırır; kaydırma, yakınlaştırma ya da sürükleme bunu kapatır, `0` ve çizgenin değişmesi yeniden açar.
- **Arama:** arama kutusunun süzgeci (liste ile aynı ayrıştırıcı, `tag:` dahil) eşleşen notları tam renkte bırakır, gerisini soldurur; etiket, eşleşen bir notunda yanar.

## Sekmeler

Editörün üstünde sekme çubuğu (ImGui `BeginTabBar`, sürükleyerek sıralanır, sığmayınca kayar). Her sekme kendi tamponunu, kirli durumunu, imlecini,
kaydırmasını ve önizleme düğmesini tutar; bilgi ve bağlantı panelleri ile haritanın merkezi etkin sekmeyi izler.
- **Açma:** ağaç/Ayrıntılar/liste/harita/bağlantı/önizleme bağlantısından açılan not etkin sekmenin yerine geçer. Etkin sekme iğneliyse, korunuyorsa ya da
  yazılmış bir taslaksa not yeni sekmede açılır. Sekme düzenlenince ya da yeni sekmede açılınca **korunur**; korunmayan sekmenin adı eğik yazılır.
  Orta tık, Ctrl+tık, satır menüsündeki **Yeni sekmede aç** ve listede/arama kutusunda `Ctrl+Enter` yeni sekmede açar (not zaten etkin sekmedeyse sekme korunur).
  Başka sekmede açık not yinelenmez, o sekme öne gelir. Yeni sekme en sona eklenir.
- **Geçiş:** ayrılan sekmenin bekleyen düzenlemesi kendi notuna yazılır (`flush_for`); kayıt olmazsa ya da vazgeçilirse geçiş olmaz. Taslak kaydedilmeden
  sekmesinde kalır (soru sorulmaz). Arka plandaki kayıtlı not hep temizdir; öne gelince dışarıda değişmişse yeniden okunur (gösterilen notun kuralı).
- **Kapatma:** `Ctrl+W`, × ya da orta tık. Kirli not kaydedilir; yazılmış taslakta "Kaydedilmemiş not" sorusu (Kaydet/At/Vazgeç), arka plandaki taslak önce öne
  getirilir. Uygulama kapanırken her yazılmış taslak sorulur. Son sekme kapanınca editör boş kalır, süzgeç kendiliğinden not açmaz.
- **Senkron ve eylemler:** taramada notu silinen (senkronda yeniden adlandırılan dahil: eski yol gider) sekme `Sekme kapandı: '<yol>' artık yok` iletisiyle
  kapanır; yalnız depo `NotFound` dediğinde (okunamayan liste sekme kapatmaz). Gösterilen kirli not mevcut akışta kalır (kayıtta taslağa döner). Klasör yeniden
  adlandırılınca sekmelerin yolu izler; çöpe atılan ya da silinen gösterilen notun sekmesi kapanır, komşu sekme öne gelir (sekme kalmadıysa en yakın not açılır).
- **Klasör seçimi:** sağda klasör özeti görünür, etkin sekme notunu tutar; sekmeye tıklamak notu geri getirir.
- **Sınır:** en çok `GuiState::kMaxTabs` = 32 sekme; aşılınca `En çok 32 sekme açılabilir` iletisi.
- **Kalıcılık:** `gui-state.toml` `[[tabs]]` (`path`, `pinned`, `kept`) sırayla ve `active_tab` (kayıtlı sekmeler içindeki sıra). Taslak yazılmaz. Açılışta
  artık olmayan ya da yinelenen yol atlanır; içerik sekme ilk öne geldiğinde okunur. Hiç sekme kalmazsa `last_note` geçerlidir.

## Kısayollar

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (fzf hissi); kutudayken `↑↓` listede gezer, `Enter` notu açıp editöre odaklanır; kutuda `Ctrl+P`/`Ctrl+N` geçmiş, `Tab` tamamlama, `→` gölge öneri (bkz. "Girdi satırı") |
| `Ctrl+Shift+M` | Arama modu: ad → içerik, içerik ve genel → ad (mod düğmesi de) |
| `Shift+E` / `Ctrl+C` | Listede ya da önizlemede: seçili not dış editörde (TUI `E`) / not baytları ya da klasör yolu panoya (TUI `c`) |
| `Ctrl+N` | Yeni not (Adsız) · `Ctrl+S` şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu (taşı/yeniden adlandır/tip değiştir) |
| `Ctrl+M` | Klasöre taşı · `Del` (listede) çöpe at, onaysız · `Shift+Del` kalıcı sil, onaylı (varsayılan Vazgeç); çöp görünümünde ikisi de kalıcı siler, onaylı |
| `Ctrl+E` | PDF dışa aktar · `F5` şimdi senkronla · `Ctrl+,` Ayarlar sekmesi |
| `Ctrl+L` | Bağlantı panelini aç ya da kapat (editör odaklıyken de; metne dokunmaz) |
| `Ctrl+I` | Bilgi panelini aç ya da kapat (editör odaklıyken de) |
| `Ctrl+Shift+V` | `md`/`markdown`/`csv`/`json` notunda önizleme ↔ düzenleme (`Ctrl+V` yapıştırmayla çakışmaz: değiştirici tam eşleşir) |
| `Ctrl+T` / `Ctrl+W` | Yeni sekmede boş taslak / etkin sekmeyi kapat |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Sonraki / önceki sekme (`Ctrl+PageDown` / `Ctrl+PageUp` de; editörde sekme karakteri yazmaz, ImGui'nin pencere geçişi kapalı) |
| `Ctrl+Enter` | Listede ya da arama kutusunda: seçili not yeni sekmede |
| `Ctrl+Shift+F` / `Ctrl+Shift+T` / `Ctrl+Shift+R` | Genel arama (ad ve içerik) / etiket seçici / kayıtlı görünüm seçici |

Eşleşme yokken arama kutusunda `Enter` → sorgu ilk satır olacak şekilde yeni not (nvALT, TUI ile aynı); argümanlı ya da bozuk sorguda yeni not açılmaz.

## Kaydetme

- **Yeni not:** ilk kayıtta (`Ctrl+S`, başka nota geçiş, pencere kapanışı) **kaydet diyaloğu** — ad (slug), klasör
  (seçili klasör), tip (`detect` önerisi + güven etiketi; açılır kutuda `types` listesi, elle uzantı yazılabilir).
  Kaydedilmemiş yeni notla kapanışta Kaydet/At/Vazgeç sorulur.
- **Mevcut not:** son tuştan `autosave_ms` (defaults.toml, 3000 ms; ölçüm ve bedel SPEC-core "Değişiklik 2026-11-02") sonra, nota geçişte ve kapanışta otomatik kaydedilir. Otomatik kayıt soru açmaz, not yaratmaz (TUI ile aynı, `app::Document`): çakışma ya da dışarıda silinen not durum çubuğunda (`Diskte değişti`/`Diskte silindi`) kalır, karar açık kayıtta (`Ctrl+S`, nota geçiş, kapanış).
- **Dışarıdan değişiklik:** kayıt `ModifiedExternally` dönerse diyalog: *Diskteki sürümü yükle* / *Benimkini yaz* /
  *Benimkini kopya olarak kaydet* (`<ad> (copy).<uzantı>`) — varsayılan düğme **kopya olarak kaydet**.
- Değişmemiş açık not dışarıda değişirse sessizce yeniden yüklenir.
- Tip kutusunu değiştirmek uzantıyı değiştirir (`move`; hedef varsa hata, ezme yok).
- **Uygulama notları (T30):** diyaloglar `GuiState::deciders` üzerinden geçer (test dikişi; `std::string` tabanlı). Diyaloglar
  ImGui modalıdır. Çakışma diyaloğunda dördüncü seçenek *Vazgeç* vardır (Esc): tampon korunur, kapanış/not geçişi iptal olur. Kayıt başka hatayla
  (ör. G/Ç) başarısız olursa tampon editörde kalır, hata durum çubuğunda görünür. Dışarıdan silinmiş kirli not,
  tampon korunarak yeni-not kaydet diyaloğuna düşer. Salt-okunur kuralı TUI ile ortaktır (`app::classify`,
  SPEC-tui "Salt okunur"): ikili, NUL içeren, geçersiz UTF-8 ve `tuning.max_note_bytes`'tan büyük not; neden
  başlıkta görünür, büyük notun başı gösterilir, hiçbir yol yazmaz. Yeni not boş (yalnız boşluk) ise
  geçişte/kapanışta sorulmadan atılır.
- **Bayt koruma (karar 4):** baştan sona CRLF olan not editörde LF görünür ve CRLF yazılır; karışık satır sonlu not
  olduğu gibi kalır (CR görünmez). Yeni not, çakışma kopyası ve silinmiş notun yeniden kaydı editördeki baytlarla
  yazılır (`create_exact`, BOM ve CRLF dahil); eskiden `create()` LF'ye çeviriyordu.
- **Yeni notun klasörü:** vurgulu klasör satırı, girilen klasör (Ayrıntılar konumu), gösterilen notun klasörü,
  yoksa `default_folder` (TUI ile ortak, `app::new_note_folder`).
- **Veri koruma (ImGui geçişi):**
  - **Escape:** ImGui varsayılanı, odaklanıldığından beri yapılan düzenlemeyi geri alır; bu sessiz veri kaybı olurdu. Tuş
    editörde işlenmez ve not metni korunur (`handle_shortcuts`).
  - **Geç NUL:** `looks_binary` yalnız ilk 8 KB'a bakar; ImGui C dizgisi işlediğinden NUL'dan sonrası kesilirdi. Metinde
    NUL varsa not salt-okunurdur.
  - **BOM:** editör tamponunda görünmez; kayıtta ve çakışma kopyasında geri yazılır.
  - **Çok büyük not:** ImGui uzunluğu `int` tutar ve salt-okunur görünümde geçersiz bayt 3 bayta büyür; `max_note_bytes = 0`
    iken de en çok `INT_MAX / 3` bayt açılır, üstü salt-okunurdur (başı gösterilir). TUI'de bu ek sınır yoktur.

## Klasörler
Ağaç bağlam menüsü: Yeni klasör, Yeniden adlandır, Sil (çöpe; doluysa not sayısıyla onay).
Not taşıma `Ctrl+M` diyaloğuyla (sürükle-bırak v1'de yok).
- **Uygulama notları (T31):** klasör adı istemi ve silme onayı `deciders` üzerinden geçer; silme onayında varsayılan
  düğme *Vazgeç*. Klasör silme `app::TrashOps::trash_folder`dır (A3, TUI ile tek kural): listedeki notlar tek tek çöpe atılır, boşalan
  klasör `remove_folder(force=false)` ile kalkar; listede olmayan not kalırsa klasör yerinde kalır, hata iletisi gelir, hiçbir şey kalıcı
  silinmez. Onaydaki sayı listedeki not sayısıdır; notsuz klasör sorulmaz. Gösterilen not etkilenen klasördeyse onaydan sonra `flush()`, vazgeçilirse hiçbir şey
  kaydedilmez. `Ctrl+M` yalnız klasörü seçtirir (ad/tip kilitli). Arama modu düğmesi mevcut modu gösterir; `Ctrl+Shift+M`
  modu çevirir (G4'ten beri kutuda `Tab` tamamlar). İçerik modunda boş sorgu tüm notları listeler. **Editör vurgusu (değişti):**
  `InputTextMultiline` metin renklendirmez; içerik aramasında nota geçince yalnız **ilk eşleşmeye** gidilir ve seçilir,
  diğer eşleşmeler boyanmaz. Seçim yalnız editör odaktayken görünür ve kayar. Seçim düzenlemede, sorgu/mod değişiminde
  silinir ve aynı (not, sorgu) için yedek taramada yeniden kaydırılmaz.

## Dosya izleme
SDL'de dizin izleyici yoktur ve yeni bağımlılık eklenmez: `rescan_seconds` aralıklı tarama + pencere odağına dönüşte
(`SDL_EVENT_WINDOW_FOCUS_GAINED`) tarama; senkron bitince de tarama. Tarama yalnız listeleme değiştiyse listeyi günceller;
seçimi ve editörü korur. Anlık (saniyenin altında) dış değişiklik algısı yoktur.

## Ayarlar sekmesi
TUI ayar ekranıyla aynı alanlar (SPEC-core config). İki sütunlu form: etiket solda hizalı (sütun en çok alanın yarısı, uzun etiket sarılır),
alan sağda ve pencereyle büyür. Gruplar: **Genel** (dil, not klasörü, varsayılan tip, varsayılan klasör, tipler, editör komutu) ve **Senkron** (aç, uzak adres,
dal, boşta bekleme, yazar adı, yazar e-postası, token, bağlantı testi) ve sonda **Görünüm** (sıra no). Alan hatası alanın altında; editör, uzak adres ve bekleme alanında kısa ipucu.
- **Görünüm (T82):** config değil, pencere tercihidir: onay kutusu hemen uygulanır ve `gui-state.toml`'a hemen yazılır; Kaydet/Geri al ve "Kaydedilmemiş değişiklikler" karşılaştırmasına girmez. Grup sonda: config alanları kaydırmadan görünür kalır.
- **Dil:** seçenekler okunur metindir (Sistem dili, Türkçe, English); saklanan değer `auto`/`tr`/`en` kalır.
- **Dosya ikonları:** satır ikonu metnin 1,3 katı çizilir, satır yüksekliği değişmez (`View::kRowIconScale`, yuva 1,6 em). Nerd Font, Kapalı; saklanan değer `nerd`/`off` (dosyadaki eski `auto` `nerd` okunur). Pencerenin kendi fontu olduğu için
  `nerd` Nerd glifi çizer, `off` çizmez: not ve çöp listesi (sabit genişlikli yuva, adlar aynı sütunda), çöp başlığı, araç çubuğu
  ("Sil" hariç: tabloda karşılığı yok). `resources/fonts/NoteappIcons.ttf` tablodaki 30 glifin alt kümesidir, UI fontuna tam
  aralıklarla birleşir (`src/gui/icon_font.*`), yalnız `noteapp_gui`'ye gömülür; `note` ve `notes` taşımaz. Yeniden üretim:
  `scripts/generate-icon-font.sh`. Lisans: `resources/fonts/LICENSE-NoteappIcons.txt`, paketlerde `licenses/noteapp-icons/`. Tablo:
  SPEC-tui "Dosya ikonları".
- **Token:** maskeli alan ve Kaydet/Sil aynı satırda, dar pencerede alta iner. Https remote'ta altında durum satırı (`kasada kayıtlı` / `yok`); başka remote'ta
  satır kilitlidir ve nedeni tek satır ipucuyla yazılır.
- **Senkron kapalıyken** aç kutusu dışındaki senkron alanları kilitlenir, değerleri korunur; kutu işaretlenince hemen açılır (kayıt akışı değişmez). Token satırı ve
  bağlantı testi kilitlenmez: https'te `enable` token ister, kasadaki sır senkron kapalıyken de silinebilmelidir; test zaten `enable` + `sync_now` çalıştırır.
- **Altbilgi:** Geri al/Kaydet kaydırılan formun dışındadır, en küçük pencerede de görünür. Form kayıtlı değerlerle aynıysa ikisi de kapalıdır; farklıysa
  "Kaydedilmemiş değişiklikler" yazar. Token bu karşılaştırmaya girmez, kendi düğmesiyle anında kasaya gider. Genel hata (bozuk config, kasa hatası) altbilgidedir.
- **Kaydedilmemiş ayarlar:** sekme değişiminde değerler formda kalır, soru sorulmaz. Pencere kapanışında Kaydet/At/Vazgeç sorulur (`deciders.close_unsaved_settings`,
  not sorusundan önce). Kayıt başarısızsa ya da Vazgeç seçilirse pencere açık kalır ve Ayarlar sekmesi öne gelir.

"Bağlantıyı test et" `enable` + `sync_now` çalıştırıp sonucu düğmenin yanında gösterir. Kaydet `config.toml`'u atomik yazar. Dil değişikliği yeniden başlatınca uygulanır (bildirilir).
- **Uygulama notları (T32):** kaydetme yalnız değişen anahtarları `set_config_value` ile uygular (hata alanın altında,
  hiçbir şey yazılmaz); bozuk `config.toml` (söz dizimi/okunamıyor) varken CLI `config set` gibi reddeder. `notes_dir`
  değişiminde önce açık not kaydedilir, sonra liste yeni dizine bağlanır. Senkron açma/`remote` değişimi `enable`
  akışını (uyuşmazlıkta onay: `deciders.confirm_replace_origin`) başarıyla bitirmeden config yazılmaz. "Bağlantıyı test
  et": `enable` UI iş parçacığında, tek `sync_now` işçide. Durum çubuğu: `senkron kapalı` / `⟳` / `✓ SS:DD` / `⚠ hata`;
  `F5` `request_now`; ayar kaydı AutoSync'i yalnız `sync.enabled/remote/branch/idle_seconds` ya da `notes_dir` değişince
  yeniden kurar (editör/yazar değişimi çalışan senkronu öldürmez), yeniden kurmadan önce bekleme imleciyle
  `stop(exit_timeout)` çağrılır (T36); her yazma/taşıma/silme/oluşturma `notify_changed`; kapanışta (kayıt akışından sonra) `stop(exit_timeout)`.

## PDF dışa aktarma
- `Ctrl+E` → yol diyaloğu: ImGui metin alanı + yerel **Browse** (SDL3 dosya diyaloğu). Önce açık not kaydedilir
  (PDF diskteki sürümden üretilir); kayıt başarısızsa dışa aktarma yapılmaz. Elle yazılan yol var olan bir dosyaysa sormadan
  ezilmez (`gui.confirm_overwrite`); Browse ile seçilince iki kez sorabilir.
  Üretim `noteapp_pdf`'tedir (A4, 15 mm, libharu + md4c, gömülü DejaVu); kapsamı SPEC-cli "PDF dışa aktarma".
  Linux'ta SDL3'ün dosya diyaloğu portal/zenity'ye dayanır; ikisi yoksa Browse davranışı doğrulanmadı, yol elle yazılabilir.
- **Başsız mod yok:** `notes-gui --export-pdf` **emekli** (2026-10-31); başsız PDF için `note export --pdf` (SPEC-cli).
- **Güvenilmez içerik (T36):** `md` HTML'siz çözülür, görseller alt metne (yoksa yer tutucu) iner ve hiçbir görsel/bağlantı
  hedefine istek yapılmaz: `file://sunucu/...` Windows'ta UNC'ye gidip NTLM özeti sızdırırdı (SPEC §9: ağ çağrısı yok),
  `file:///...` yerel görseli PDF'e gömerdi. Baytlar `NoteStore::read`'den gelir, dosya sistemine inilmez (G4).
- **Açılış uyarıları (T36):** config yükleme uyarıları (bozuk `config.toml`, geçersiz değer, ayıklanan kimlik bilgisi)
  durum çubuğunda gösterilir (`show_config_warnings`).

## Diğer
- **Tema:** yalnız açık/koyu ayrımı `SDL_GetSystemTheme` ile izlenir (değişince güncellenir); pencere KDE/Windows temasını
  izlemez, yerel görünüm yoktur. Tüm eylemler klavyeyle erişilebilir.
- **Ekran okuyucu desteği yok:** ImGui erişilebilirlik ağacı sunmaz. Eski "erişilebilir adlar" maddesi emekli. `gui.a11y_*`
  katalog anahtarları kullanılmıyor (silinmedi).
- **Fontlar:** yalnız gömülü DejaVu Sans / Sans Mono; önizleme ayrıca PDF'in gömülü Bold / Oblique / BoldOblique yüzlerini yükler. DejaVu'da olmayan glifler (CJK, renkli emoji) ekranda `?` kutusu
  görünür; dosya baytları bozulmaz. BMP dışı kod noktaları metin işlemede tek kod noktası sayılır (`IMGUI_USE_WCHAR32`).
- Pencere boyutu, sol bölme genişliği, gezgin görünümü, sıra no, son seçili not: `<durum>/gui-state.toml` (`width`, `height`, `left_pane_width`,
  `map_pane_width`, `view_mode`, `row_numbers`, `last_note`; açık sekmeler: bkz. "Sekmeler"; toml++). Bölme ölçek 1 birimindedir, sınırlanır: sağ panel ve not listesi pencere dışına itilmez.
  Eski kayıttaki `folder_pane_height` (ayrı klasör bölmesi) okunmaz, yok sayılır.
  Config değil; bozuksa varsayılana düşülür. `imgui.ini` yazılmaz.
  Eski `gui-state.ini` okunmaz: pencere boyutu ve son not bir kez sıfırlanır, not verisi etkilenmez.
- Metinler core i18n katalogundan (`tr()`); tek i18n kaynağı.
- `info` diyaloğu başlığı sabit "noteapp".

## Kabul kriterleri (`SDL_VIDEO_DRIVER=dummy` + yazılımsal çizim, `SDL_RENDER_DRIVER=software`; Xvfb gerekmez)

Testler `ctest` altında bu ortam değişkenleriyle koşar; çizim testlerinde tuşlar `io.AddKeyEvent` ile verilir. `perf_tests`
doğrudan koşulursa aynı ortam elle verilmelidir.

1. Yeni not → kaydet diyaloğu `detect` önerisiyle dolu; seçilen tip uzantıya yansır.
2. Mevcut notta yazma → `autosave_ms` sonra diskte; aradaki tuşlar tek yazmaya birleşir.
3. Dışarıdan değişmiş notta kayıt → diyalog; "kopya olarak kaydet" iki dosyayı da korur.
4. Dolu klasör silme onaysız olmaz; onayla çöpe atar, Geri al notları ve klasörü getirir (A3: kalıcı silme yalnız çöpten).
5. ~~`--export-pdf` geçerli, boş olmayan bir PDF üretir; hatalı girdide çıkış 1.~~ **EMEKLİ (2026-10-31):** `--export-pdf` kalktı.
   Karşılığı: GUI'de `Ctrl+E` geçerli PDF üretir (`tests/gui/pdf_test.cpp`); başsız PDF doğrulaması SPEC-cli kabul 1–10 ve
   `tests/pdf`'dedir.
6. Ayarlar sekmesinde kaydedilen değer `note config get` ile okunur; token config dosyasında geçmez.
7. 5.000 notla arama kutusunda tuş başına liste güncellemesi ≤ 50 ms (ölçüm 2026-10-31: 2.6 ms; 1 MB notta tuş medyanı
   16.7 ms, plan riski sınırı 50 ms aşılmadı; boşta CPU 30 sn'de 0.24 sn, odaktaki editörde ölçülmedi).
8. Escape, geç NUL ve BOM koruması (yukarıdaki "Veri koruma") korunur; raporlara göre mutasyonla doğrulandı. >= 2 GiB yolu için
   otomatik test kaydı bulunamadı (doğrulanmadı).
   Ekran okuyucu ve renkli editör vurgusu kabul kapsamı dışıdır (emekli davranış).
9. Türkçe uzun yol, uzun senkron hatası ve uzun durum iletisiyle 1 / 1.25 / 1.5 / 2 ölçekte, 1000x650 ve en küçük pencerede: iki sekme ve diyaloglarda adlı
   öğeler çakışmaz, pencere dışına çıkmaz (`tests/gui/layout_test.cpp`). Görüntü aracı: `gui_layout_shots` (ctest dışı).
10. (T82) Bilgi paneli TUI alanlarını, tipi ve etiketleri gösterir, kayıttan sonra boyut tazelenir (`tests/gui/info_test.cpp`); sıra no
    klasör başına 1'den numaralar, ayar `gui-state.toml`'da kalır, Ayarlar'daki kutu Ağaç ve Ayrıntılar'da sütunu açar
    (`row_numbers_test.cpp`); önizleme başlık, liste, kod, alıntı, tablo ve bağlantıyı kaynak sözdizimi olmadan çizer, `[[bağlantı]]`
    tıklaması bekleyen düzenlemeyi kendi notuna yazıp hedefi açar (`preview_test.cpp`; md4c yürüyüşü `tests/pdf/markdown_blocks_test.cpp`,
    çözüm kuralı `tests/core/note_links_test.cpp`); bilgi paneli açık, sıra no açık ve önizleme her ölçekte en küçük pencerede de sığar
    (`layout_test.cpp`). Görüntüler: `gui_layout_shots -tc="renders the info panel*"`.
11. (T84) Harita: düğüm ve kenarlar, eksik hedef çizilmez; kenarlar değişiklik başına bir kez okunur, kare başına okunmaz; çevre derinliği; konumlar yeniden
    taramada korunur; tıklama bekleyen düzenlemeyi yalnız kendi notuna yazıp tıklanan notu açar; çift tıklama Ağaç'a geçer; sürükleme iğneler; `tag:` vurgusu;
    oklar, `+`/`-`, `0`; görünüm yeniden açılışta kalır; yerleşim durunca kare üretilmez (`tests/gui/map_test.cpp`). Yerleşim belirlenimci, yakınsar (enerji
    azalır, NaN yok, düğümler ayrık), iğne ve yeniden kurulum; kapsam, sınır ve etiket (`graph_layout_test.cpp`). Denetimler her ölçekte en küçük pencerede
    sığar, düğümler tuvalde (`layout_test.cpp`). **Ölçüm** (Release, dummy + yazılımsal çizici, 1000x650, 2.001 not, 2.000 düğüm, 4.999 kenar, sınır uyarısı
    görünür; `gui_tests -tc="map perf*"`, yalnız Release'te derlenir): iki koşu, yerleşim sürerken kare 19,2–20,5 ms, durunca 16,3–17,1 ms (bütçe 33,3 ms =
    30 fps); yerleşim adımı kare başına en kötü 3,1–3,2 ms (bütçe `View::kMapLayoutBudgetMs` 8 ms). Debug'da çizici -O0'dır, ölçülmez. Görüntüler: `gui_layout_shots -tc="renders the map*"`.

**Elle (yapılmadı):** Windows 11 (çapraz derlenmiş `.exe`) ve Fedora KDE/GNOME — Türkçe klavye (`ı/İ`, dead key, Ctrl kısayolları),
IME, koyu tema, yerel dosya diyaloğu, PDF'te Türkçe karakterlerin gözle kontrolü, senkron durumunun durum çubuğuna yansıması,
Windows'ta temiz makinede açılış (MinGW çalışma zamanı DLL'leri kopyalanmıyor; çözülmedi). Gerçek ekranda HiDPI ölçeği ve
`SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` (dummy sürücüde ölçek hep 1; olay ve ölçekler elle verilerek sınandı).

## Kapsam dışı
WYSIWYG düzenleme, önizlemede görsel, HTML ve dış bağlantı açma, editörde sözdizimi renklendirme (G5), sürükle-bırak, çoklu pencere/sekme, anlık dil değişimi, sistem tepsisi,
ekran okuyucu desteği, dizin izleyici (OS bildirimi).

## Değişiklik 2026-10-31

Qt 6 Widgets yerine SDL3 + Dear ImGui. Karar ve gerekçe: `tasks/plan-imgui.md` (toolkit kararı kullanıcının).

- **MODIFIED** Yerleşim, editör, diyaloglar: ImGui anlık mod; `GuiState` + `View` ayrımı; kısayol tablosu aynı.
- **MODIFIED** Dosya izleme: `QFileSystemWatcher` yerine `rescan_seconds` taraması + pencere odağına dönüşte tarama.
- **MODIFIED** Editör içerik vurgusu: yalnız ilk eşleşme seçilir (editör odaktayken), diğerleri boyanmaz; liste vurgusu kalın değil renkli.
- **MODIFIED** Durum dosyası: `gui-state.ini` yerine `gui-state.toml`; bir kez sıfırlanır.
- **MODIFIED** Tema: yalnız açık/koyu (`SDL_GetSystemTheme`); yerel görünüm yok.
- **MODIFIED** Kabul ortamı: `QT_QPA_PLATFORM=offscreen` yerine `SDL_VIDEO_DRIVER=dummy`.
- **MODIFIED** PDF üretimi `noteapp_pdf`'e taşındı; yalnız soldan sağa yazı. Yol alanı elle yazılabilir + Browse; üzerine yazma onayı.
- **ADDED** Veri koruma: Escape, geç NUL, BOM, >= 2 GiB. Neden: ImGui varsayılanları (Escape geri alma, C dizgisi, `int` uzunluk) sessiz veri kaybı doğururdu.
- **ADDED** Gömülü DejaVu; glif kapsamı sınırlı (CJK, renkli emoji `?` kutusu).
- **REMOVED** Ekran okuyucu desteği ("erişilebilir adlar").
- **REMOVED** `notes-gui --export-pdf` ve kabul 5 (numara korundu, emekli).
- **REMOVED** `windeployqt` sonrası açılış kontrolü; yerine MinGW DLL riski elle listesinde.

## Değişiklik 2026-11-01

- **MODIFIED** Ölçek: birincil ekranın `SDL_GetDisplayContentScale`'i yerine pencerenin ölçeği; ekran/ölçek değişiminde yeniden uygulanır. Neden: birincil ekran penceredeki ekran olmayabilir.
- **MODIFIED** Bölme boyutları ölçek 1 biriminde saklanır; eski kayıtlar ölçek 1 varsayılır. Neden: 2x'te 320 piksellik sol panel okunmaz daralıyordu.
- **MODIFIED** Sığmayan metin kısalır, tam metin ipucunda; düğme sıraları alta iner; diyalog pencereyle sınırlı. Neden: durum çubuğu metinleri üst üste biniyor, tip kutusu ve düğmeler pencere dışına çıkıyordu.
- **ADDED** En küçük pencere 640x420 x ölçek. Neden: yerleşim bu boyutun altında bozulur.
- **ADDED** Araç çubuğu ve boş liste iletisi. Neden: eylemler yalnız kısayol ve sağ tıkta olunca keşfedilemiyordu; davranış eklenmedi.
- **ADDED** Kabul 9: çakışma testi. Tip kutusunun pencere içinde kalması artık otomatik sınanıyor.
- **FIXED** Diyalog ve ipucu arka planı opak: arkadaki metin içinden görünüyordu.
- **FIXED** Ayarlar etiketleri ham anahtar (`gui_setting_language`) yerine katalog metni gösterir.

## Değişiklik 2026-11-02

- **MODIFIED** Ayrı klasör bölmesi ve sol panel içindeki araç çubuğu kalktı: klasörler ve notlar tek ağaçta (varsayılan) ya da Ayrıntılar tablosunda; araç çubuğu iki bölmenin üstünde tek ve seçili satıra uygulanır. `folder_pane_height` yok sayılır, `view_mode` eklendi.
- **ADDED** Ayrıntılar görünümü (Yukarı, yol düğmeleri), görünüm seçici, klasör özeti, araç çubuğunda Yeniden adlandır.

## Değişiklik 2026-11-02 (T82)

- **ADDED** Bilgi paneli (`Ctrl+I`): TUI'nin satır ayrıntısı alanları, tip ve etiketler. Neden: GUI'de notun zamanları, senkron durumu ve kaynağı görünmüyordu.
- **ADDED** Sıra no (Ayarlar → Görünüm, `gui-state.toml` `row_numbers`): Ağaç ve Ayrıntılar'da TUI kuralıyla numara.
- **ADDED** `md` önizlemesi (`Ctrl+Shift+V`, başlık düğmesi): PDF'in md4c yürüyüşüyle; `[[bağlantı]]` tıklanır. `NoteStore::resolve_link` ve `pdf::MarkdownOptions` eklendi; PDF çıktısı değişmedi.
- **MODIFIED** Kapsam dışı: "Markdown önizleme" çıkarıldı; WYSIWYG ve önizlemede görsel/HTML/dış bağlantı kapsam dışı kalır.

## Değişiklik 2026-11-02 (T84)

- **ADDED** Harita görünümü (görünüm seçicide üçüncü düğme, `view_mode = "map"`, `map_pane_width`): notlar, bağlantı, ilişki ve isteğe bağlı etiketler kuvvet
  yerleşimli çizgede; çevre derinliği, 2.000 düğüm sınırı, tıklama/çift tıklama/sürükleme, kaydırma ve yakınlaştırma, arama vurgusu. Neden: kullanıcı isteği
  ("not haritası"); bağlantı paneli tek notun komşularını gösteriyordu.
- **ADDED** `NoteStore::edges()`: tüm canlı notlar arası kenarlar tek okumada (salt okuma API'si; yazma yolu değişmedi).

## Değişiklik 2026-11-03 (T85)

- **ADDED** Editör sekmeleri (bkz. "Sekmeler"; `Ctrl+T`, `Ctrl+W`, `Ctrl+Tab`, `Ctrl+PageUp/PageDown`, `Ctrl+Enter`, orta tık / Ctrl+tık), `gui-state.toml`
  `[[tabs]]` ve `active_tab`. Neden: kullanıcı isteği ("sekmeler"); aynı anda tek not açılabiliyordu.
- **MODIFIED** Yazılmış taslaktan başka nota geçmek kayıt diyaloğu açmaz: taslak kendi sekmesinde kalır, not yeni sekmede açılır. Ctrl+N da korunan sekmede
  yeni sekme açar. Önizleme düğmesi oturum boyunca değil sekme başınadır. Klasör silinince gösterilen notun sekmesi iletiyle kapanır.

## Değişiklik 2026-11-03 (A1)

- **MODIFIED** Kısayollar, listedeki `Del`/`Shift+Del`, araç çubuğu, görünüm seçici, çöp düğmeleri ve arama kipi düğmesi eylemlerini `src/gui/commands.hpp`'deki tablodan kimlikle alır; kimlikler TUI ile ortak kayıttandır (`src/app/commands.hpp`). Davranış ve tuşlar aynıdır. Palet/yardım ekranı G3'te.
- **ADDED** Eşitlik bekçisi (`tests/gui/command_parity_test.cpp`): kayıttaki her komut GUI'de bağlı ya da `tests/support/command_parity_gaps.hpp`'de sahipli eksiktir.

## Değişiklik 2026-11-03 (R3)

- **MODIFIED** Liste, ağaç ve Ayrıntılar satırlarını `app::Listing` kurar (TUI ile tek kural); GUI yalnız kök satırını ve çizimi ekler. Neden: plan-parity R3.
- **MODIFIED** Ağaç ve Ayrıntılar artık adla değil kullanıcının sırasıyla, varsayılan en yeni başta (karar 6). Açık klasörler oturumluk değil, `gui-state.toml` `open_folders`'ta kalır.
- **ADDED** Sırala düğmesi ve menüsü, genel arama (`Ctrl+Shift+F`), etiket ve görünüm seçicileri (`Ctrl+Shift+T`, `Ctrl+Shift+R`), `sync:` için not başına senkron durumu (işçi `src/app`'e taşındı). Eşitlik bekçisinin GUI eksik listesinden `sort`, `sort.*`, `search.global`, `search.tag`, `search.view` çıktı.

## Değişiklik 2026-11-03 (A3)

- **MODIFIED** Klasör silme kalıcı değil, çöpe atar: notlar tek tek çöpe, boşalan klasör kalkar; Geri al notları ve boş alt klasörleri getirir. Notsuz klasör onaysız. Kalıcı silme yalnız çöpten. Neden: plan-parity karar 1; önceki `remove_folder(force=true)` notları geri dönüşsüz siliyordu.
- **MODIFIED** Çöp, geri alma, geri getirme, kalıcı silme ve onay kuralı `app::TrashOps`'tan (TUI ile ortak, `src/app/trash_ops.hpp`).
- **MODIFIED** Geri al bir yığındır: her basış bir önceki çöpe atmayı (klasör adımı bütün) geri getirir; teklif "Geri getirildi" iletisiyle, yığında adım kaldıkça sürer. Yığın oturumluktur. Neden: plan-parity karar 2; önceki GUI yalnız son notu geri alıyordu.
- **ADDED** `limits.app.trash_undo_steps` (varsayılan 0 = sınırsız): geri alma yığınının derinliği, TUI ile aynı anahtar; dolunca en eski adım düşer (not çöpte kalır).

## Değişiklik 2026-11-03 (A5)

Ortak belge modeli (plan-parity A5, `src/app/document.hpp`).

- **MODIFIED** Salt-okunur kuralı TUI ile aynı: `tuning.max_note_bytes` üstü not salt-okunur açılır (önceden yalnız
  >= 2 GiB); neden başlıkta görünür. NUL ve geçersiz UTF-8 arındırılmış metinle gösterilir.
- **MODIFIED** Baytlar korunur (karar 4): yeni not ve çakışma kopyası `create_exact` ile yazılır, CRLF ve BOM kalır
  (önceden kopya ve yeni not LF'ye çevriliyordu, kopyadan sonraki kayıtlar da LF yazıyordu). Karışık satır sonlu not
  LF'ye indirilmez (önceden kayıtta bütün satırlar CRLF oluyordu).
- **MODIFIED** Otomatik kayıt soru açmaz (TUI ile aynı): çakışmada diyalog yerine durum çubuğu iletisi, silinen notta
  kaydet diyaloğu yerine ileti; açık kayıt sorar.
- **MODIFIED** Yeni notun klasörü TUI ile tek kural; hiçbir şey seçili değilken `default_folder` (önceden kök).
- **MODIFIED** Çakışma seçenekleri `app::ConflictChoice`; kopya adı ve dosya adı bölme `core::split_note_path`.

## Değişiklik 2026-11-03 (G4)

Önizleme, dış editör, pano, açılış ipucu ve arama satırı eşitliği (plan-parity G4).

- **ADDED** CSV ve JSON önizlemesi (bkz. "Önizleme"); ayrıştırma `src/app/data_preview.*`'a taşındı (A7), TUI aynısını kullanır. Sınırlar TUI'nin anahtarları: `limits.tui.view_bytes`, `view_lines`, `table_rows`, `table_columns`. Ad `limits.view.*` olmadı: o tablo kayıtlı Cypher görünümlerinindir (`[limits.view]`).
- **MODIFIED** Önizleme sınırı sabit 256 KiB (`GuiState::kMaxPreviewBytes`) değil `limits.tui.view_bytes`.
- **ADDED** `external` (`Shift+E`, not satırı menüsü, palet; TUI `E`): bekleyen düzenleme önce yazılır; `core::run_editor` (config `editor` → `$VISUAL` → `$EDITOR` → platform) işçide beklenir, pencere donmaz; not `NoteStore::edit_in_temp_file` ile (özel geçici dosya, çakışma denetimi) geri yazılır, dönüşte tarama temiz gösterilen notu yeniden okur. Editör hatası söylenir, yazdığı bayt yine kaydedilir (TUI ile aynı). Aynı anda tek dış editör; açıkken pencere kapanmaz (`Dış editör hâlâ açık`). Terminal editörü (`nano`, `vi`) masaüstünden başlatılan GUI'de terminalsiz kalır: GUI'de bekleyen bir pencere editörü (`code --wait`, `gedit`) yapılandırılmalı.
- **ADDED** `copy` (`Ctrl+C` listede ve önizlemede, not satırı menüsü, palet; TUI `c`): klasör satırında yol, notta özgün baytlar (CRLF, BOM dahil) SDL panosuna; ikili ve geçersiz UTF-8 not reddedilir, nedeni durum çubuğunda. Eşitlik bekçisinin GUI eksik listesinden `external` ve `copy` çıktı.
- **ADDED** Açılışta geçiş dışı dosya ipucu (matris 84): `app::count_stray_files` (TUI ile ortak kural, `limits.tui.stray_notice_*`) işçide kendi depo bağlantısıyla sayar; sayı > 0 ise durum çubuğunda `note graph status` ipucu, önceki ileti (config uyarısı) varsa onu ezmez.
- **ADDED** Arama kutusu TUI girdi satırı (bkz. "Girdi satırı"): geçmiş, `Tab` tamamlama ve listesi, gölge öneri, renkli argümanlar.
- **MODIFIED** Arama modu `Tab` yerine `Ctrl+Shift+M` (kutuda `Tab` artık tamamlar, TUI gibi). Kutuda `Ctrl+P` geçmiştir (odak zaten kutuda), `Ctrl+N` yalnız geçmişte gezerken.
- **ADDED** Durum çubuğunda işaret sayısı (`seçili: 3`, TUI `x` sayacı). Toplu silme gösterilen notu götürünce onu tek notluk silme gibi bırakır: sekmesi kapanır, imleç aynı sıradaki nota iner (önceden tarama seçimi listenin başına düşürüyordu).
