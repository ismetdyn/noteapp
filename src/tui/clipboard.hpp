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
    // Base64'ten önce; 100 KB'lik OSC 52 dizisi yaygın terminallerin güvenli sınırıdır.
    static constexpr std::size_t kOsc52MaxBytes = 75'000;
    static constexpr std::size_t kMaxPasteBytes = std::size_t{32} * 1024 * 1024;
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

    explicit SystemClipboard(TerminalWriter terminal = write_to_terminal,
                             std::chrono::milliseconds timeout = kToolTimeout);

    CopyResult copy(std::string_view text);
    Pasted paste();
    // Son kopya araca ulaşmadıysa sistem panosu eskidir: yapıştırma iç panoyu seçer.
    bool stale() const { return stale_; }

private:
    TerminalWriter terminal_;
    std::chrono::milliseconds timeout_;
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

}  // namespace noteapp::tui
