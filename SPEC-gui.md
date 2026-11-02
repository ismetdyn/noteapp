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
- Sol: arama kutusu + mod düğmesi (Ad/İçerik) → klasör ağacı (kök "Tümü"; seçili klasör listeyi alt ağaçla
  sınırlar) → araç çubuğu → not listesi (göreli yollar, eşleşen karakterler renkli vurgulu — kalın değil; içerik modunda eşleşme sayısı;
  liste boşsa not oluşturmayı anlatan tek ileti).
- **Araç çubuğu:** düz metin düğmeler (DejaVu'da simge yok), mevcut eylemleri çağırır: Yeni not, Yeni klasör (seçili klasörün içinde), Sil (onaysız çöpe atar; durum çubuğunda ileti ve Geri al), Çöp (n) (liste panelinde çöp görünümü), Taşı, PDF'e aktar. İpucunda kısayol. Sil seçili not yoksa; Taşı gösterilen kayıtlı not yoksa; PDF ise ikili notta, boş taslakta (`Ctrl+E` onu atardı) ve
  not yokken kapalıdır. Dar panelde alt satıra iner. Bağlam menüleri ve kısayollar kalır.
- **Çöp görünümü (`Çöp (n)`):** liste paneli çöpü gösterir, üstünde başlık ve Geri getir, Kalıcı sil, Boşalt (ikisi onaylı, varsayılan Vazgeç), Geri düğmeleri. `↑↓` çöp satırlarını gezer; `Del` ve `Shift+Del` onaylı kalıcı siler. Arama yazmak, klasör seçmek ya da yeni not çöpten çıkarır. Sağ panel açık notta kalır; çöp önizlemesi ve satır ayrıntısı GUI kapsamı dışında.
- Sağ: başlık şeridi (yol, **tip açılır kutusu**, kaydedildi/değişti göstergesi) → `InputTextMultiline` (gömülü DejaVu Sans Mono).

## Kısayollar

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (fzf hissi); kutudayken `↑↓` listede gezer, `Enter` notu açıp editöre odaklanır |
| `Ctrl+N` | Yeni not (Adsız) · `Ctrl+S` şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu (taşı/yeniden adlandır/tip değiştir) |
| `Ctrl+M` | Klasöre taşı · `Del` (listede) çöpe at, onaysız · `Shift+Del` kalıcı sil, onaylı (varsayılan Vazgeç); çöp görünümünde ikisi de kalıcı siler, onaylı |
| `Ctrl+E` | PDF dışa aktar · `F5` şimdi senkronla · `Ctrl+,` Ayarlar sekmesi |

Eşleşme yokken arama kutusunda `Enter` → sorgu ilk satır olacak şekilde yeni not (nvALT, TUI ile aynı).

## Kaydetme

- **Yeni not:** ilk kayıtta (`Ctrl+S`, başka nota geçiş, pencere kapanışı) **kaydet diyaloğu** — ad (slug), klasör
  (seçili klasör), tip (`detect` önerisi + güven etiketi; açılır kutuda `types` listesi, elle uzantı yazılabilir).
  Kaydedilmemiş yeni notla kapanışta Kaydet/At/Vazgeç sorulur.
- **Mevcut not:** son tuştan `autosave_ms` (defaults.toml, 3000 ms; ölçüm ve bedel SPEC-core "Değişiklik 2026-11-02") sonra, nota geçişte ve kapanışta otomatik kaydedilir.
- **Dışarıdan değişiklik:** kayıt `ModifiedExternally` dönerse diyalog: *Diskteki sürümü yükle* / *Benimkini yaz* /
  *Benimkini kopya olarak kaydet* (`<ad> (copy).<uzantı>`) — varsayılan düğme **kopya olarak kaydet**.
- Değişmemiş açık not dışarıda değişirse sessizce yeniden yüklenir.
- Tip kutusunu değiştirmek uzantıyı değiştirir (`move`; hedef varsa hata, ezme yok).
- **Uygulama notları (T30):** diyaloglar `GuiState::deciders` üzerinden geçer (test dikişi; `std::string` tabanlı). Diyaloglar
  ImGui modalıdır. Çakışma diyaloğunda dördüncü seçenek *Vazgeç* vardır (Esc): tampon korunur, kapanış/not geçişi iptal olur. Kayıt başka hatayla
  (ör. G/Ç) başarısız olursa tampon editörde kalır, hata durum çubuğunda görünür. Dışarıdan silinmiş kirli not,
  tampon korunarak yeni-not kaydet diyaloğuna düşer. Geçerli UTF-8 olmayan ve ikili dosyalar salt-okunurdur
  (geri yazım baytları bozardı). CRLF'li dosya CRLF olarak kaydedilir. Yeni not boş (yalnız boşluk) ise
  geçişte/kapanışta sorulmadan atılır.
- **Veri koruma (ImGui geçişi):**
  - **Escape:** ImGui varsayılanı, odaklanıldığından beri yapılan düzenlemeyi geri alır; bu sessiz veri kaybı olurdu. Tuş
    editörde işlenmez ve not metni korunur (`handle_shortcuts`).
  - **Geç NUL:** `looks_binary` yalnız ilk 8 KB'a bakar; ImGui C dizgisi işlediğinden NUL'dan sonrası kesilirdi. Metinde
    NUL varsa not salt-okunurdur.
  - **BOM:** editör tamponunda görünmez; kayıtta ve çakışma kopyasında geri yazılır.
  - **>= 2 GiB not:** ImGui uzunluğu `int` tutar; bu notlar yer tutucuyla salt-okunur açılır.

## Klasörler
Ağaç bağlam menüsü: Yeni klasör, Yeniden adlandır, Sil (doluysa not sayısıyla onay; kalıcı silme uyarısı).
Not taşıma `Ctrl+M` diyaloğuyla (sürükle-bırak v1'de yok).
- **Uygulama notları (T31):** klasör adı istemi ve silme onayı `deciders` üzerinden geçer; silme onayında varsayılan
  düğme *Vazgeç*. Klasör silmede onaydaki sayı `remove_folder(force=false)`'ın `NotEmpty {count}` değeridir (gizli/geçersiz adlı
  dosyalar dahil, `.keep` hariç; T36); sonra zorla silinir. Listede not yoksa önce sade "boş klasör" onayı sorulur
  (boş klasör onaysız silinmesin); gizli dosya çıkarsa yalnız o durumda sayıyla ikinci onay gelir. Gösterilen not etkilenen klasördeyse önce `flush()`, vazgeçilirse hiçbir şey
  kaydedilmez. `Ctrl+M` yalnız klasörü seçtirir (ad/tip kilitli). Arama modu düğmesi mevcut modu gösterir; arama
  kutusunda `Tab` modu çevirir. İçerik modunda boş sorgu tüm notları listeler. **Editör vurgusu (değişti):**
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
dal, boşta bekleme, yazar adı, yazar e-postası, token, bağlantı testi). Alan hatası alanın altında; editör, uzak adres ve bekleme alanında kısa ipucu.
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
- **Fontlar:** yalnız gömülü DejaVu Sans / Sans Mono. DejaVu'da olmayan glifler (CJK, renkli emoji) ekranda `?` kutusu
  görünür; dosya baytları bozulmaz. BMP dışı kod noktaları metin işlemede tek kod noktası sayılır (`IMGUI_USE_WCHAR32`).
- Pencere boyutu, bölme genişlik/yükseklikleri, son seçili not: `<durum>/gui-state.toml` (`width`, `height`, `left_pane_width`,
  `folder_pane_height`, `last_note`; toml++). Bölmeler ölçek 1 birimindedir, sınırlanır: sağ panel ve not listesi pencere dışına itilmez.
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
4. Dolu klasör silme onaysız olmaz; onayla siler.
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

**Elle (yapılmadı):** Windows 11 (çapraz derlenmiş `.exe`) ve Fedora KDE/GNOME — Türkçe klavye (`ı/İ`, dead key, Ctrl kısayolları),
IME, koyu tema, yerel dosya diyaloğu, PDF'te Türkçe karakterlerin gözle kontrolü, senkron durumunun durum çubuğuna yansıması,
Windows'ta temiz makinede açılış (MinGW çalışma zamanı DLL'leri kopyalanmıyor; çözülmedi). Gerçek ekranda HiDPI ölçeği ve
`SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` (dummy sürücüde ölçek hep 1; olay ve ölçekler elle verilerek sınandı).

## Kapsam dışı
Markdown önizleme/WYSIWYG, sözdizimi renklendirme, sürükle-bırak, çoklu pencere/sekme, anlık dil değişimi, sistem tepsisi,
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
