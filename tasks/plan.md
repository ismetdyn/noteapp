# Uygulama Planı: noteapp v1

> Kaynak: [SPEC.md](../SPEC.md) + `SPEC-<modül>.md` (onaylandı 2026-10-28) · Görevler: [todo.md](todo.md)
> Durum: **ONAYLANDI (2026-10-28)** · Commit: görev başına, `feat/v1` dalında

## Genel bakış

Yedi modül (core, detect, search, sync, cli, tui, gui) haritadaki bağımlılık sırasıyla, ama **dikey dilimlerle**
inşa edilir: her faz sonunda kullanıcının deneyebileceği çalışan bir ürün vardır. İlk dilim `note "metin"`
(core + cli), sonra CLI'nin geri kalanı, TUI, senkron, GUI. Riskli araç zinciri işleri (libgit2/SSH, Qt, FTXUI,
Fedora derlemesi) en başta spike'larla çözülür (erken başarısız ol).

## Mimari kararlar

1. **Tek CMake projesi:** modül başına statik kütüphane (`noteapp_<modül>`), ön yüz başına çalıştırılabilir,
   modül başına doctest çalıştırılabiri. Uyarılar-hata-olarak yalnız bizim hedeflere uygulanır (üçüncü parti hariç).
2. **Tüm bağımlılıklar Faz 0'da** `cmake/Dependencies.cmake`'e pin'lenir; sonraki görevler bu dosyaya dokunmaz →
   paralel görevler aynı dosyada çakışmaz.
3. **Windows araç zinciri:** Qt'nin MinGW 13.1'i + Qt 6.12.0 `mingw_64`. Makineye özel yollar
   `CMakeUserPresets.json`'da (gitignore'da). Test/çalıştırma için Qt ve MinGW `bin` dizinleri preset ortamında PATH'e eklenir.
4. **libgit2 her platformda kaynaktan** (FetchContent, `USE_SSH=exec`) — Fedora'da da sistem paketi değil;
   böylece SSH davranışı her yerde aynı. Fedora'da `openssl-devel` + `libsecret-devel` gerekir.
5. **Kaynak gömme:** `resources/*.toml` derlemede CMake ile üretilen `.cpp`'ye ham dizgi olarak gömülür;
   metinlerin tek kaynağı yine dosyalardır (hard-code yasağı), ikili tek başına taşınabilir kalır.
6. **Süreç başlatma tek yerde:** `core::process` (CreateProcessW / posix_spawnp, kabuksuz) — editör, arka plan
   senkronu ve `notes-gui --export-pdf` çağrısı bunu kullanır.
7. **Test edilebilir ön yüzler:** TUI'de saf `TuiState` modeli + metne render edilen FTXUI görünümü; GUI'de ince
   Qt katmanı, `QT_QPA_PLATFORM=offscreen` ile doctest. CLI testleri ikiliyi alt süreç olarak çalıştırır
   (`NOTEAPP_HOME` geçici dizin, `fake-editor` test yardımcısı).
8. **Ağsız testler:** senkron testleri yerel bare repolarla; GitHub'a gerçek erişim yalnız elle doğrulamada.
9. **Ağır ritüel (TDD Demir Kural, önce kırmızı test)** yalnız kritik yollarda: `NotePath`, `NoteStore` yazma,
   senkron çakışma/birleştirme, token yönetimi. Diğerlerinde hafif TDD.

## Fazlar ve görevler (ayrıntı: todo.md)

### Faz 0 — Temel ve risk spike'ları
- T01 Derleme iskeleti (CMake, preset'ler, doctest/toml++, ilk test)
- T02 Fedora Docker doğrulaması (`scripts/check-fedora.sh`)
- T03 Spike: libgit2 + keychain (MinGW + Fedora), SSH-exec doğrulaması
- T04 Spike: FTXUI + Qt (Türkçe metin, offscreen PDF)
- **Kontrol noktası 0:** araç zinciri riskleri kapandı; Windows + Fedora yeşil

### Faz 1 — core + ilk dikey dilim `note "metin"`
- T05 `Error` + `NotePath::parse` · T06 UTF-8/transliterasyon/slugify · T07 `NoteStore` temel
- T08 Kaynak gömme + config + platform dizinleri · T09 i18n + log · T10 CLI hızlı ekleme
- **Kontrol noktası 1:** `note "merhaba dünya"` çalışıyor (≤ 200 ms)

### Faz 2 — CLI'nin geri kalanı
- T11 detect + korpus + CLI'ye bağlama · T12 `core::process` + editör · T13 CLI editör akışı
- T14 taşı/sil/klasör + `rm`/`mv` · T15 fuzzy + `ls`/`show`/`edit` · T16 `ContentIndex` + `find` · T17 `config` komutları
- **Kontrol noktası 2:** CLI tam (senkron/PDF hariç); edit/micro ile elle akış

### Faz 3 — TUI
- T18 `TuiState` modeli · T19 FTXUI görünümü · T20 içerik modu · T21 editör + kaydet diyaloğu
- T22 taşı/tip/sil + yardım · T23 ayarlar ekranı
- **Kontrol noktası 3:** `notes` Windows Terminal'de elle denendi; 5.000 notla performans

### Faz 4 — Senkron
- T24 temel döngü + kilit · T25 birleştirme + çakışma kopyaları · T26 `enable` + kimlik doğrulama
- T27 `AutoSync` + `note sync …` · T28 TUI senkron entegrasyonu
- **Kontrol noktası 4:** gerçek GitHub (HTTPS+token ve SSH) elle test; security-review

### Faz 5 — GUI
- T29 iskelet (sekmeler, liste, arama, ağaç) · T30 kaydetme akışları · T31 klasör işlemleri + içerik araması
- T32 Ayarlar sekmesi + senkron · T33 PDF dışa aktarma (GUI + başsız + CLI)
- **Kontrol noktası 5:** GUI elle (Windows); Fedora konteynerinde offscreen testler

### Faz 6 — Bitiriş
- T34 Kurulum + windeployqt + README · T35 Performans doğrulaması · T36 Son review (code-review + security-review)
- **Son kontrol noktası:** SPEC.md §10 başarı kriterlerinin tümü işaretli

## Paralelleştirme

| Paralel yapılabilir | Neden güvenli |
|---|---|
| T05 ∥ T06 | Farklı dosyalar, saf fonksiyonlar |
| T11 (detect) ∥ T12–T14 | detect bağımsız modül |
| T15–T16 (search) ∥ T12–T14 | Yalnız core'un sabitlenmiş arayüzüne dayanır |
| Faz 3 (TUI) ∥ T24–T26 (sync) | Ayrı modüller; T28'de birleşir |

Sıralı olması gerekenler: Faz 0 → her şey; T07 → T14; T10 → tüm CLI görevleri; `src/cli/commands.cpp`'ye
dokunan görevler (T13–T17, T27, T33) aynı anda yapılmaz.

## Ajan/model dağıtımı (CLAUDE.md §3)
Net speckli 1–2 dosyalık görevler → sonnet; çok dosyalı/entegrasyon (T07, T10, T19, T21, T24–T27, T29–T30) →
sonnet; mimari karar gerektiren spike'lar (T03, T04) ve son tüm-dal review (T36) → opus. Model her dağıtımda açıkça verilir.

## Riskler ve önlemler

| Risk | Etki | Önlem |
|---|---|---|
| libgit2 `USE_SSH=exec` Windows/MinGW'de çalışmaz | Yüksek | T03 spike en başta; olmazsa libssh2 (önce sorulur) |
| libgit2/keychain MinGW 13.1 ile derlenmez | Yüksek | T03'te görülür; yama/derleme bayrağı, gerekirse vcpkg'yi sor |
| FTXUI editörü askıya alıp geri dönmede Windows konsolu bozulur | Orta | T04/T21'de `WithRestoredIO` + `edit`/`micro` ile elle test |
| GCC 13'te C++23 eksikleri | Düşük | `std::print` kullanılmaz; T01'de `std::expected` derleme testi |
| Fedora'ya özgü hatalar geç fark edilir | Orta | T02 ile her kontrol noktasında Docker'da Fedora derleme + test |
| Keychain Linux'ta Secret Service yokken çalışmaz (konteyner/başsız) | Orta | `NOTEAPP_GIT_TOKEN` yolu; keychain testleri `[manual]` etiketli, varsayılan koşuda yok |
| Senkronda veri kaybı | Yüksek | Ağır TDD ritüeli; çakışma senaryoları bare repolarla; force push/hard reset kodda yok (review'da kontrol) |
| Performans hedefleri tutmaz | Orta | T35'te 5.000 üretilmiş notla Release ölçümü; darboğazda önce profil |
| Kurum ağı (proxy) FetchContent'i engeller | Orta | T01'de görülür; olursa sana sorulur (yerel ayna/önbellek) |

## Senden gerekenler (yürütme sırasında)

1. **Kontrol noktası 0'dan önce:** Docker Desktop'ın çalışıyor olması.
2. **Kontrol noktası 4'te:** boş bir **private** GitHub reposu, bir fine-grained PAT (yalnız o repo, Contents
   read/write) ve SSH anahtarının GitHub'a ekli olması. Token'ı ben görmeden `note sync login` ile kendin girersin.
3. **Elle testler:** kontrol noktalarında Windows Terminal'de kısa denemeler; evde Fedora'da son kontrol.

## Açık sorular
- ~~Commit politikası~~ — **Karar:** spec+plan `main`e ilk commit; sonra `feat/v1` dalında görev başına bir commit, push yok.
