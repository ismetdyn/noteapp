# ADR 0002 — PDF işçisinin sınırı: askıya ve çöküşe karşı, yetkiye karşı değil

- Durum: yerini [ADR 0003](0003-pdf-worker-sandbox.md) aldı (kabul edilmişti: 2026-11-09)
- Kapsam: `src/pdfview/pdf_document.cpp` (istemci), `src/pdfview/engine/worker.cpp` (`note __pdf-worker`),
  `src/core/child_process.cpp` (başlatma); tasarım SPEC-core.md "PDF yardımcı süreci"

## Bağlam

PDF'ler senkronla gelir, güvenilmez girdidir. MuPDF bu girdiyi ayrıştırır; bazı döngüleri kesilemez (onarım,
bağlantı hedefinin yavaş sayfa araması, Form XObject çoğalması) ve C kodudur: bellek hatası ya da istismar
olasılığı sıfır değildir. Kullanıcı kararıyla (2026-11-09) ayrıştırma belge başına bir `note __pdf-worker`
sürecine taşındı; ebeveyn süre, iptal, çöküş ve bozuk çerçevede işçiyi öldürür.

## Karar

İşçi süreci **askıda kalmaya ve çöküşe karşı bir sınırdır, yetki sınırı değildir.**

- Sağladığı: kesilemeyen döngü süreyle ya da durdurmayla öldürülür; çöken işçi uygulamayı düşürmez; işçinin her
  cevabı katı uzunluk ve sınır denetiminden geçer, metin yeniden temizlenir, ayrıntı yalnız loga
  (`terminal_safe`) gider. Bozuk ya da düşmanca bir işçi ebeveynde bellek taşması, sınır aşımı ya da log
  sahteciliği yapamaz.
- Sağlamadığı: işçi kum havuzunda değildir. Kullanıcının hesabıyla, ebeveynin ortamıyla (senkron token'ı ve
  AppImage değişkenleri düşer) ve devralınan fd'lerle çalışır. MuPDF'te kod çalıştırmayı başaran bir PDF
  kullanıcının yapabildiği her şeyi yapar: `~/.ssh`'i ve kimlik bilgilerini okur, `/proc/<ebeveyn pid>/environ`'u
  (aynı kullanıcı) okur, not deposunu değiştirir, ağa çıkar.

Kum havuzu bu turda uygulanmaz.

## Sonuçlar

- Güvenlik iddiası "PDF uygulamayı kilitleyemez ya da düşüremez"dir; "PDF kod çalıştıramaz" değildir. Belgeler
  ve inceleme bunu böyle söyler.
- MuPDF sürümünün güncel tutulması (güvenlik düzeltmeleri) kod çalıştırma riskine karşı tek önlemdir.

## Ertelenen işler

1. OS kum havuzu: Linux'ta seccomp (izinli sistem çağrısı listesi: okuma/yazma/çıkış/bellek) ve Landlock
   (dosya sistemine erişim yok); Windows'ta kısıtlı token (düşük bütünlük düzeyi) ve iş nesnesi sınırları
   (UI, süreç sayısı, bellek).
2. Ortam allowlist'i: işçiye yalnız gereken değişkenler geçer (bugün yalnız token ve AppImage değişkenleri düşer).
3. Devralınan fd'lerin kapatılması: işçide yalnız 0, 1, 2 açık kalır.
4. OS bellek sınırı (rlimit, iş nesnesi; SPEC-core'da ertelendi).

## Yeniden değerlendirme koşulu

Ertelenen 1–3 uygulandığında bu ADR'nin yerine yenisi yazılır; ya da MuPDF'te kod çalıştırma açığı
yayımlandığında, ya da uygulama geniş dağıtıma çıkmadan önce.
