# Spec: gui

> Modül: `gui` (`notes-gui` çalıştırılabiri, Qt 6 Widgets) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

Aynı notları masaüstü penceresinde oluşturmak, düzenlemek, klasörlerle düzenlemek ve klavyeyle hızlıca
gezinmek; ayarları bir sekmeden yönetmek; PDF dışa aktarmak.

## Yerleşim

`QMainWindow` → `QTabWidget`: **Notlar** | **Ayarlar**. Durum çubuğu: senkron durumu, not sayısı.

**Notlar sekmesi** (yatay splitter):
- Sol: arama kutusu + mod düğmesi (Ad/İçerik) → klasör ağacı (kök "Tümü"; seçili klasör listeyi alt ağaçla
  sınırlar) → not listesi (göreli yollar, eşleşme vurgulu; içerik modunda eşleşme sayısı).
- Sağ: başlık şeridi (yol, **tip açılır kutusu**, kaydedildi/değişti göstergesi) → `QPlainTextEdit` (eş aralıklı font).

## Kısayollar

| Tuş | Eylem |
|---|---|
| `Ctrl+K` / `Ctrl+P` | Arama kutusuna odak (fzf hissi); kutudayken `↑↓` listede gezer, `Enter` notu açıp editöre odaklanır |
| `Ctrl+N` | Yeni not (Adsız) · `Ctrl+S` şimdi kaydet |
| `Ctrl+Shift+S` | Ad/klasör/tip diyaloğu (taşı/yeniden adlandır/tip değiştir) |
| `Ctrl+M` | Klasöre taşı · `Del` (listede) sil — onaylı |
| `Ctrl+E` | PDF dışa aktar · `F5` şimdi senkronla · `Ctrl+,` Ayarlar sekmesi |

Eşleşme yokken arama kutusunda `Enter` → sorgu ilk satır olacak şekilde yeni not (nvALT, TUI ile aynı).

## Kaydetme

- **Yeni not:** ilk kayıtta (`Ctrl+S`, başka nota geçiş, pencere kapanışı) **kaydet diyaloğu** — ad (slug), klasör
  (seçili klasör), tip (`detect` önerisi + güven etiketi; açılır kutuda `types` listesi, elle uzantı yazılabilir).
  Kaydedilmemiş yeni notla kapanışta Kaydet/At/Vazgeç sorulur.
- **Mevcut not:** son tuştan `autosave_ms` (defaults.toml, 1000 ms) sonra, nota geçişte ve kapanışta otomatik kaydedilir.
- **Dışarıdan değişiklik:** kayıt `ModifiedExternally` dönerse diyalog: *Diskteki sürümü yükle* / *Benimkini yaz* /
  *Benimkini kopya olarak kaydet* (`<ad> (copy).<uzantı>`) — varsayılan düğme **kopya olarak kaydet**.
- Değişmemiş açık not dışarıda değişirse sessizce yeniden yüklenir.
- Tip kutusunu değiştirmek uzantıyı değiştirir (`move`; hedef varsa hata, ezme yok).
- **Uygulama notları (T30):** diyaloglar `MainWindow::deciders` üzerinden geçer (test dikişi). Çakışma diyaloğunda
  dördüncü seçenek *Vazgeç* vardır (Esc): tampon korunur, kapanış/not geçişi iptal olur. Kayıt başka hatayla
  (ör. G/Ç) başarısız olursa tampon editörde kalır, hata durum çubuğunda görünür. Dışarıdan silinmiş kirli not,
  tampon korunarak yeni-not kaydet diyaloğuna düşer. Geçerli UTF-8 olmayan ve ikili dosyalar salt-okunurdur
  (geri yazım baytları bozardı). CRLF'li dosya CRLF olarak kaydedilir. Yeni not boş (yalnız boşluk) ise
  geçişte/kapanışta sorulmadan atılır.

## Klasörler
Ağaç bağlam menüsü: Yeni klasör, Yeniden adlandır, Sil (doluysa not sayısıyla onay; kalıcı silme uyarısı).
Not taşıma `Ctrl+M` diyaloğuyla (sürükle-bırak v1'de yok).
- **Uygulama notları (T31):** klasör adı istemi ve silme onayı `deciders` üzerinden geçer; silme onayında varsayılan
  düğme *Vazgeç*. Klasör silmede sayım not sayısıdır; notsuz görünen klasörde not olmayan dosya çıkarsa dosya
  sayısıyla ikinci onay istenir. Gösterilen not etkilenen klasördeyse önce `flush()`, vazgeçilirse hiçbir şey
  kaydedilmez. `Ctrl+M` yalnız klasörü seçtirir (ad/tip kilitli). Arama modu düğmesi mevcut modu gösterir; arama
  kutusunda `Tab` modu çevirir. İçerik modunda boş sorgu tüm notları listeler; editör vurgusu düzenlemede,
  sorgu/mod değişiminde silinir ve aynı (not, sorgu) için yedek taramada yeniden kaydırılmaz.

## Dosya izleme
`QFileSystemWatcher` (notes_dir dizinleri) + `rescan_seconds` aralıklı yedek tarama; senkron çekmesi sonrası yenileme.

## Ayarlar sekmesi
TUI ayar ekranıyla aynı alanlar (SPEC-core config). Token: maskeli alan + Kaydet/Sil, "kasada kayıtlı" göstergesi,
yalnız https remote'ta etkin. "Bağlantıyı test et" düğmesi `enable` + `sync_now` çalıştırıp sonucu gösterir.
Kaydet/Geri al düğmeleri; kaydet `config.toml`'u atomik yazar. Dil değişikliği yeniden başlatınca uygulanır (bildirilir).

## PDF dışa aktarma
- `Ctrl+E` → dosya diyaloğu. `md` → `QTextDocument::setMarkdown`; diğer tipler → `setPlainText` + eş aralıklı font.
  `QPdfWriter`, A4, 15 mm kenar boşluğu; Türkçe glifler sistem fontuyla gömülür.
- **Başsız mod:** `notes-gui --export-pdf <girdi> <çıktı>` → pencere açmadan dışa aktarır; çıkış 0/1, hata stderr'e (i18n).
  `QT_QPA_PLATFORM=offscreen` ile çalışır (CLI bunu kullanır).

## Diğer
- Sistem temasını (açık/koyu) izler. Tüm eylemler klavyeyle erişilebilir; widget'ların erişilebilir adları i18n.
- Pencere geometrisi, splitter oranı, son seçili not: `<durum>/gui-state.ini` (`QSettings` IniFormat). Config değil.
- Metinler core i18n katalogundan (`tr()`); Qt'nin `.ts/.qm` sistemi kullanılmaz (tek i18n kaynağı).

## Kabul kriterleri (`QT_QPA_PLATFORM=offscreen` testleri)

1. Yeni not → kaydet diyaloğu `detect` önerisiyle dolu; seçilen tip uzantıya yansır.
2. Mevcut notta yazma → `autosave_ms` sonra diskte; aradaki tuşlar tek yazmaya birleşir.
3. Dışarıdan değişmiş notta kayıt → diyalog; "kopya olarak kaydet" iki dosyayı da korur.
4. Dolu klasör silme onaysız olmaz; onayla siler.
5. `--export-pdf` geçerli, boş olmayan bir PDF üretir (`%PDF-` başlığı); hatalı girdide çıkış 1.
6. Ayarlar sekmesinde kaydedilen değer `note config get` ile okunur; token config dosyasında geçmez.
7. 5.000 notla arama kutusunda tuş başına liste güncellemesi ≤ 50 ms.

**Elle:** Windows 11 ve Fedora (GNOME/Wayland) — Türkçe giriş, koyu tema, PDF'te Türkçe karakterler,
senkron durumunun durum çubuğuna yansıması, `windeployqt` sonrası temiz makinede açılış.

## Kapsam dışı
Markdown önizleme/WYSIWYG, sözdizimi renklendirme, sürükle-bırak, çoklu pencere/sekme, anlık dil değişimi, sistem tepsisi.
