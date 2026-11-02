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
};

}  // namespace noteapp::tui
