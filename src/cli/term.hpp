#pragma once

// Terminal kararlarının tek yeri (SPEC-cli "Terminal"): akış TTY mi, renk açık mı, pencere boyutu, ilerleme
// satırı.
#include <string_view>

namespace noteapp::cli {

enum class Stream { Out, Err };

// stdin gerçek bir terminal mi (Windows'ta NUL aygıtı TTY sayılmaz).
bool stdin_is_tty();
bool stream_is_tty(Stream stream);

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
