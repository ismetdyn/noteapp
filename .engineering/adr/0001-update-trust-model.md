# ADR 0001 — Kendi kendini güncellemenin güven modeli

- Durum: kabul edildi (2026-11-09)
- Kapsam: `src/app/updater.cpp` (denetim, indirme), `src/app/update_install.cpp` (kurulum),
  `scripts/make-release.sh` (release varlıkları)

## Bağlam

`note update`, TUI ve GUI GitHub'daki son release'i indirip kurar. Kurulan paket kullanıcının hesabında kod
çalıştırır; paketin kaynağına ne kadar güvenildiği açıkça yazılmalı.

## Karar

Bir paket şu iki koşul birlikte sağlanınca kurulur:

1. **TLS:** API cevabı, SHA256SUMS ve paket yalnız HTTPS ile, sertifika doğrulaması açıkken indirilir (Linux
   OpenSSL, Windows Schannel; doğrulama asla kapatılmaz). Bütün indirme adresleri aynı release'in
   `https://github.com/ismetdyn/noteapp/releases/download/<tag>/` önekini taşımalıdır.
2. **Aynı release'teki SHA256SUMS:** paketin SHA-256'sı, aynı release'in `SHA256SUMS` dosyasındaki tek satırla
   eşleşmelidir. Ayrıca yalnız daha yeni sürüm kurulur ve kurulan paket release sürümünü bildirmelidir (yerel
   kurulumda `install-local.sh --expect-version`, Windows'ta `note.exe --version`).

**İmza yok.** SHA256SUMS paketle aynı yerden (aynı GitHub hesabından) gelir. Bu yüzden indirmedeki bozulmaya ve
yanlış varlığa karşı korur, ama ele geçirilmiş bir GitHub hesabına ya da release'e karşı korumaz: hesabı ele geçiren
kişi geçerli SUMS'lu bir paket yayımlar, güncelleyen her kullanıcıda kod çalıştırır. Kullanıcı imzalamayı bu turda
erteledi.

## Sonuçlar

- Güvenlik GitHub hesabının (2FA, token kapsamı) ve release sürecinin güvenliğine bağlıdır.
- Kurulum kullanıcı onayı ister. Kaynaktan derleme, sistem geneli ve yazılamayan kurulum kendini güncellemez.
- TUI ve GUI'nin açılış denetimi varsayılan olarak açıktır (en çok 24 saatte bir): api.github.com'a IP adresi ve
  `User-Agent: noteapp/<sürüm>` gider. `update.check_interval = 0` kapatır (UPD5).
- İmza eklemek varlık sözleşmesini değiştirir (ör. `SHA256SUMS.sig` ve uygulamaya gömülü açık anahtar), bu ADR'nin
  yerine yenisi yazılır.

## Yeniden değerlendirme koşulu

Uygulama geniş dağıtıma çıkmadan önce (paket deposu, web sitesinden indirme, kurum içi yaygın kullanım) ya da
kullanıcı imzalamayı onayladığında.
