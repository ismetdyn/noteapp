#pragma once

// Terminal kararlarının tek yeri (SPEC-cli "Terminal"): akış TTY mi, renk açık mı, pencere boyutu, ilerleme
// satırı.
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::cli {

enum class Stream { Out, Err };

// stdin gerçek bir terminal mi (Windows'ta NUL aygıtı TTY sayılmaz; mintty/MSYS/Cygwin pty'si sayılır).
bool stdin_is_tty();
bool stream_is_tty(Stream stream);

// Windows'ta mintty/MSYS2/Cygwin pty'si konsol değil, adlı bir borudur (isatty/_isatty false döner); adı
// `\msys-<anahtar>-pty<N>-...` ya da `\cygwin-<anahtar>-pty<N>-...` kalıbındadır. Ad
// GetFileInformationByHandleEx(FileNameInfo)'dan gelir; çözümleme saf olsun diye her platformda derlenir.
bool is_msys_pty_pipe_name(std::wstring_view name);

// Sır satırı ister (token). `echo_hidden`: terminal ekosu gerçekten kapandı. Kapandıysa `prompt` `out`a yazılır
// ve `in`den bir satır okunur. Kapanamadıysa (Windows mintty/MSYS pty konsol değildir) görünür istemde sır
// istenmez: yalnız `refusal` yazılır, hiçbir şey okunmaz, nullopt.
std::optional<std::string> read_hidden_line(std::istream& in, std::ostream& out, std::string_view prompt,
                                             std::string_view refusal, bool echo_hidden);

// İlk eşleşen kazanır: NOTEAPP_COLOR=always|never; NO_COLOR boş değilse kapalı; TERM=dumb kapalı; akış TTY
// değilse kapalı; aksi açık. stdout ve stderr ayrı karar verir (biri boruya gidebilir).
bool color_enabled(Stream stream);

// stderr'de tek satırlık "çalışıyor" iletisi (SPEC-cli "İlerleme"): ağ ya da çok dosya beklerken kullanıcı
// sessiz bir terminale bakmasın. Yalnız stderr TTY ve TERM=dumb değilken yazılır; nesne ölünce satır silinir
// (sonraki çıktı temiz satırdan başlar). Boru, dosya ve dumb terminal hiçbir ara çıktı görmez.
class ProgressLine {
public:
    explicit ProgressLine(std::string_view text);
    ~ProgressLine();
    void update(std::string_view text);  // satırı silip yeni metni yazar (sayaç)
    ProgressLine(const ProgressLine&) = delete;
    ProgressLine& operator=(const ProgressLine&) = delete;

private:
    bool shown_ = false;
};

struct TermSize {
    unsigned columns = 0;  // 0: bilinmiyor
    unsigned rows = 0;
};

// stdout'un terminali; bilinmiyorsa COLUMNS/LINES, o da yoksa 0.
TermSize term_size();

}  // namespace noteapp::cli
