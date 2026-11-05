# Plan: arayüz yenileme (GUI + TUI + CLI) ve boşta çizim yok

> Durum: **ONAYLANDI** (kullanıcı 2026-11-05: "Hepsi sırayla") · İstek 2026-11-04: "gui çok çirkin… sadece değişen kısımları render et… tui ve cli için de"
> Kullanıcı kararları: K1 font = "üçü de olsun" → DejaVu (varsayılan, mevcut) + Inter (UI) + JetBrains Mono (editör) gömülür, Ayarlar'dan seçilir (OFL; yeni varlık onaylı). K2 Notlar/Ayarlar üst sekmesi kalkar (⚙ + Ctrl+,). K3 araç çubuğu "Yeni not" dışında yalnız simge + ipucu. Lead varsayılanları: K4 TUI truecolor teması yok (YAGNI), K5 önizleme başlık ölçekleri SPEC'te güncellenir.
> Kaynaklar: `.engineering/design/ui-redesign.md` (GUI/TUI, ekran görüntülü), `.engineering/design/cli-redesign.md` (CLI), cpp-architect render raporu (bu dosyada özet).

## Render kararı (lead)
- Bölge bazlı çizim (dirty-rect) yok: `SDL_RenderPresent` her zaman tam kare sunar, ImGui draw list'i her karede kurar. Doku önbelleği ölçüm 8 ms bütçeyi aşmadıkça yok.
- Hedef: değişiklik yoksa hiç kare yok. GUI boşta (editör odakta da) 10 sn'de 0 kare, CPU < %0,5; TUI boşta 0 bayt ve 0 çizim; yazma gecikmesi değişmez.

## Fazlar (en çok 2 derleyen ajan)
- **R — boşta çizim yok:** GUI `tick` boş taramada uyandırmaz (`gui_state.cpp:364`), editör odakta imleç zamanlayıcısı yok (`view.cpp:312`, LSP gönderimi korunur); TUI `RequestAnimationFrame` yerine `next_wake` zamanlayıcısı (`tui/main.cpp:688,735,751`). Kare sayacı testleri.
- **U1 — tema (yerleşime dokunmaz):** GUI G1 tema modülü + token'lar (WCAG AA kontrast testi), G2 sabit renkler token'a; TUI T4 sözdizimi aile hizası, T1 yuvarlak çerçeve + odak rengi, T2 modal kenar birleşme hatası. Önce/sonra PNG kullanıcıya gösterilir.
- **U2 — yerleşim:** G3 tipografi, G4 ikonlar, G5 üst çubuk, G6 kutu içinde kutu biter, G7 tek durum çubuğu, G8 liste satırı, G9 editör, G10 önizleme ölçüsü, G11 diyalog düğmeleri, G12 palet, G13 harita, G14 boş/hata durumları; TUI T3 üst/alt satır + ipucu çubuğu, T5 palet satırı.
- **C — CLI:** T1 renk/TTY (`NO_COLOR`), T2–T3 "did you mean" + hata ipucu, T4–T5 alt komut `--help` + gruplu yardım (80 sütun), T6 hızlı eklemede yazım ipucu, T7 uzun bayraklar `--yes`/`-q`, T8 `ls --json`/`--long`, T9 pager, T10–T11 küçükler + ilerleme, T12 belge. Varsayılan stdout, `--json` şemaları ve çıkış kodları değişmez.

## Kabul
Her iş: önce düşen test, iki arayüzde eşitlik, `layout_test` 1/1,25/1,5/2 ölçek, pty 120×40 ve 60×15, `NO_COLOR` yakalaması, kaynak kuralları; faz sonunda review + Linux/Wine/AppImage.
