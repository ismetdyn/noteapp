# Spec: tui

> Modül: `tui` (`notes` çalıştırılabiri, FTXUI) · Bağımlılık: core, detect, search, sync · Harita: [SPEC.md](SPEC.md) · Durum: ONAYLANDI (2026-10-28)

## Amaç

`notes` yazınca açılan, klavyeyle fzf hızında gezinilen iki panelli not tarayıcısı.

## Yerleşim

```
┌───────────────────────────────────────────────────────────────────────┐
│ > sorgu▏                                     [ad] 42/5000   ✓ 14:32    │
├──────────────────────────────┬────────────────────────────────────────┤
│ ▸ work/meeting-notes.md      │ # Toplantı notları                     │
│   personal/todo.md           │ - bütçe onayı                          │
│   snippets/sort.java         │ ...                                    │
├──────────────────────────────┴────────────────────────────────────────┤
│ Enter aç  Tab içerik  ^O yeni  ^R ad/klasör/tip  ^D sil  ^S senk  F2 ayarlar  F1 yardım │
└───────────────────────────────────────────────────────────────────────┘
```
- Sol: göreli yol listesi (klasörler yolun parçası), eşleşen karakterler vurgulu. Sağ: seçili notun önizlemesi.
- Üst: sorgu, mod (`[ad]` / `[içerik]`), sonuç/toplam, senkron durumu (`kapalı` / `✓ saat` / `⟳` / `⚠`).
- Alt: kısayol çubuğu (i18n). En küçük boyut 60×15; altında "pencere çok küçük" mesajı.

## Tuşlar

| Tuş | Eylem |
|---|---|
| yazı | Sorguyu düzenler, liste anında süzülür |
| `↑` `↓` `Ctrl-P` `Ctrl-N` | Listede gezin |
| `PgUp` `PgDn` | Önizlemeyi kaydır |
| `Tab` | Ad ↔ içerik modu |
| `Enter` | Seçili notu editörde aç; **eşleşme yoksa** sorgu ilk satır olacak şekilde yeni not (nvALT) |
| `Ctrl-O` | Boş yeni not (editörde) |
| `Ctrl-R` | Seçili not için ad/klasör/tip diyaloğu (taşı/yeniden adlandır/tip değiştir) |
| `Ctrl-D` | Sil (onay diyaloğu, varsayılan seçim "Vazgeç") |
| `Ctrl-S` | Şimdi senkronla |
| `F2` | Ayarlar ekranı · `F1` yardım |
| `Esc` | Sorgu doluysa temizle, boşsa çık · `Ctrl-C` çık |

İçerik modunda liste not başına eşleşme sayısını gösterir; önizleme ilk eşleşmeye kayar ve eşleşmeleri vurgular.

## Editör ve kaydet diyaloğu

- Editör, FTXUI terminali bırakarak (`WithRestoredIO`) çalışır; kapanınca ekran geri gelir ve liste yenilenir.
- Yeni not akışı: geçici dosya → editör → içerik boşsa sessizce iptal → değilse **kaydet diyaloğu**:
  - **Ad:** slug ile dolu, düzenlenebilir.
  - **Klasör:** varsayılan `default_folder`, varsa seçili notun klasörü; yazarken mevcut klasörler fuzzy önerilir;
    yeni klasör adı yazılabilir.
  - **Tip:** `detect` önerisiyle dolu (`öneri: java %82`); `←`/`→` ile `types` listesinde gezilir ya da uzantı yazılır.
  - `Enter` kaydet. `Esc` → "Not atılsın mı?" onayı (veri kaybını önler); hayırsa diyaloğa döner.
- `Ctrl-R` aynı diyaloğu mevcut not için açar; kaydet = `NoteStore::move` (hedef varsa hata, ezme yok).
  - Sapma (T22): taşıma kipinde `detect` ipucu gösterilmez (ilk değer mevcut tip); `Esc` onaysız iptal eder (atılacak metin yok).

## Ayarlar ekranı (F2)

SPEC-core config alanlarının formu: dil (döngü), notes_dir, editör, varsayılan tip (döngü), varsayılan klasör,
senkron açık/kapalı, remote, dal, idle süresi, yazar adı/e-posta; remote https ise **token** alanı (maskeli;
"kasada kayıtlı ✓" göstergesi, Kaydet/Sil). Kaydet → doğrula → `config.toml` atomik yazılır → uygulanır
(dil anında; notes_dir değişirse liste yeniden yüklenir; senkron açıldıysa `enable` akışı, hata satır içinde).
Vazgeç → değişiklikler atılır.

## Yenileme ve senkron
- Liste: editör dönüşünde, senkron çekme sonrası ve `rescan_seconds` (defaults.toml, 5 sn) aralıkla
  (yalnız stat; değişiklik yoksa yeniden çizim yok).
- `AutoSync`: açılışta başlar, her kayıtta `notify_changed`, çıkışta `stop(exit_timeout)`; durum `screen.Post` ile.

## Kabul kriterleri

1. Ekranı metne render eden testler: ilk liste, filtrelenmiş liste (vurgu), içerik modu, kaydet diyaloğu, ayarlar, küçük pencere.
2. Durum modeli (`TuiState`, saf) testleri: tuş → durum geçişleri; eşleşme yokken Enter'ın yeni not akışını tetiklemesi; Esc mantığı.
3. Kaydet diyaloğunda tip önerisi dolu gelir, değiştirilen tip dosya uzantısına yansır; Esc'te onaysız içerik atılmaz.
4. Silme yalnız onayla; varsayılan seçim "Vazgeç".
5. 5.000 notla açılış ≤ 500 ms, tuş başına filtre ≤ 50 ms (SPEC.md §10).
6. Ayarlar ekranında kaydedilen değer `config.toml`'da ve `note config get` çıktısında görünür.

**Elle:** Windows Terminal + Fedora (GNOME Terminal) — Türkçe karakter girişi/görünümü, `edit`/`micro`/`nano`/`vim`
ile askıya alma/geri dönme, pencere yeniden boyutlandırma.

## Kapsam dışı
TUI içinde metin düzenleme (editör harici), fare desteği (FTXUI varsayılanı dışında), klasör silme/yeniden adlandırma
(GUI'de), PDF dışa aktarma (CLI/GUI'de), tema seçimi.
