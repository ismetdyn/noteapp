#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace noteapp::tui {

// Sistem panosu: Windows'ta Win32, Wayland'de wl-copy/wl-paste, X11'de xclip ya da xsel, macOS'ta
// pbcopy/pbpaste; kopyalamada yedek OSC 52. Araçlar kabuksuz çalışır, veri stdin'den gider, süre sınırlıdır.
// Araç yoksa ya da takılırsa hiçbir şey çökmez ve metin kaybolmaz: iç pano EditorHost'ta kalır.
class SystemClipboard {
public:
    // Base64'ten önce; 100 KB'lik OSC 52 dizisi yaygın terminallerin güvenli sınırıdır. Terminal protokolünün
    // sınırıdır, config'te değil: aşan kopya iç panoda kalır, ileti pano aracının (wl-copy, xclip, xsel)
    // kurulmasını önerir.
    static constexpr std::size_t kOsc52MaxBytes = 75'000;
    // `limits.tui.paste_bytes` varsayılanı.
    static constexpr std::size_t kDefaultMaxPasteBytes = std::size_t{32} * 1024 * 1024;
    // Takılan araç en çok bu kadar bekletir (UI iş parçacığı); araç normalde milisaniyelerde döner.
    static constexpr std::chrono::milliseconds kToolTimeout{1500};

    enum class CopyResult { Tool, Terminal, TooLarge, Binary };
    enum class PasteStatus { Text, NoTool, Unavailable, Binary, TooLarge };
    struct Pasted {
        PasteStatus status = PasteStatus::Unavailable;
        std::string text = {};
    };

    using TerminalWriter = std::function<void(std::string_view)>;
    static void write_to_terminal(std::string_view sequence);

    // `max_paste_bytes` (sınırsız SIZE_MAX): bundan büyük pano yapıştırılmaz, TooLarge döner.
    explicit SystemClipboard(TerminalWriter terminal = write_to_terminal,
                             std::chrono::milliseconds timeout = kToolTimeout,
                             std::size_t max_paste_bytes = kDefaultMaxPasteBytes);

    CopyResult copy(std::string_view text);
    Pasted paste();
    // Son kopya araca ulaşmadıysa sistem panosu eskidir: yapıştırma iç panoyu seçer.
    bool stale() const { return stale_; }
    std::size_t max_paste_bytes() const { return max_paste_bytes_; }

private:
    TerminalWriter terminal_;
    std::chrono::milliseconds timeout_;
    std::size_t max_paste_bytes_;
    bool stale_ = false;
};

// Kopyalama sonucunun kullanıcıya gösterilecek metni.
std::string copy_message(SystemClipboard::CopyResult result, std::size_t bytes);

struct PasteChoice {
    std::string text;     // boşsa yapıştırılacak bir şey yok
    std::string message;  // boşsa ileti yok
};
// `Ctrl+V` kararı: sistem panosu, yoksa ya da kullanılamıyorsa iç pano (`internal`).
PasteChoice choose_paste(SystemClipboard& clipboard, const std::string& internal);

// Terminalin yapıştırdığı metin (`Ctrl+Shift+V`, bracketed paste): CRLF, tek CR ve LF birer LF olur. Terminaller
// satır sonunu çoğunlukla CR'ye çevirir (Konsole, VTE, xterm), bazıları CRLF'i aynen gönderir; notun satır
// sonunu tampon koyar (EditorBuffer::paste).
std::string terminal_paste_text(std::string_view raw);
// Terminal yapıştırmasının kararı: `Ctrl+V` ile aynı sınır (`limits.tui.paste_bytes`, sınırsız SIZE_MAX).
// Aşan metin normalleştirilmeden reddedilir (büyük yapıştırmanın ikinci kopyası kurulmaz), ileti nedeni söyler.
PasteChoice terminal_paste(std::string_view raw, std::size_t max_bytes);
// `limits.tui.paste_bytes`'ı aşan yapıştırmanın iletisi (Ctrl+V ve terminal aynı metni gösterir).
std::string paste_too_large_message(std::size_t max_bytes);

}  // namespace noteapp::tui
