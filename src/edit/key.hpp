#pragma once

namespace noteapp::tui {

// Terminal kütüphanesinden bağımsız tuş soyutlaması: FTXUI olayları main.cpp'de buna çevrilir, böylece
// model hiçbir UI başlığına bağlanmadan test edilir.
enum class Key {
    Char,  // yazdırılabilir karakter; metni handle()'a ayrıca verilir
    Backspace,
    Up,
    Down,
    CtrlP,
    CtrlN,
    PageUp,
    PageDown,
    Tab,
    ShiftTab,
    Left,
    Right,
    Enter,
    Escape,
    CtrlO,
    CtrlR,
    CtrlD,
    CtrlS,
    F1,
    F2,
    CtrlC,
    // Yalnız gömülü editörün kullandığı tuşlar; Liste ve Arama bunları yok sayar.
    Home,
    End,
    Delete,
    ShiftUp,
    ShiftDown,
    ShiftLeft,
    ShiftRight,
    ShiftHome,
    ShiftEnd,
    CtrlHome,
    CtrlEnd,
    CtrlX,
    CtrlV,
    CtrlY,
    CtrlZ,
    CtrlF,
    F3,
    CtrlQ,
    CtrlSpace,
    AltLeft,   // satır üst verisini kapatır
    AltRight,  // satır üst verisini açar
    F9,
    CtrlU,
    CtrlT,
    CtrlA,  // Liste: görünen tüm satırları seçer
    // Sekmeler (SPEC-tui "Sekmeler"): Ctrl+T yeni, Ctrl+W kapat, Alt+N / Alt+P (Ctrl+PgDn / Ctrl+PgUp)
    // sonraki / önceki; Ctrl+K editörden sekmeyi açık bırakıp aramaya geçer.
    CtrlW,
    CtrlK,
    AltN,
    AltP,
    CtrlPageDown,
    CtrlPageUp,
    // Panel yakınlaştırma (pane.zoom): her odakta, editörde de; harf `z` metne aittir. Alt+Z (`ESC z`) önce:
    // GNOME Terminal ve Konsole F11'i kendi tam ekranına ayırır.
    AltZ,
    F11,
    // Terminalden yapıştırılan metin (bracketed paste) tek olaydır; metni (satır sonları LF) handle()'a ayrıca
    // verilir. Tuş tuş gelseydi CRLF iki Enter, Tab öneri kabulü, harfler Listede kısayol olurdu.
    Paste,
};

}  // namespace noteapp::tui
