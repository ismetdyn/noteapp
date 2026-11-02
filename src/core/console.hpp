#pragma once

namespace noteapp::core {

// Windows'ta konsolun giriş/çıkış kod sayfasını UTF-8'e çeker, yıkıcıda eskisini geri yükler (kullanıcının
// konsolu, ör. cmd'de 857, değişmiş kalmasın). Ayrıca stdout konsoldaysa ANSI/VT dizilerini (vurgu renkleri)
// açar ve yıkıcıda eski kipi geri yükler. POSIX'te ve konsola bağlı olmayan süreçte etkisizdir.
class [[maybe_unused]] ConsoleUtf8Scope {
public:
#ifdef _WIN32
    ConsoleUtf8Scope();
    ~ConsoleUtf8Scope();
#else
    ConsoleUtf8Scope() = default;
    ~ConsoleUtf8Scope() = default;
#endif
    ConsoleUtf8Scope(const ConsoleUtf8Scope&) = delete;
    ConsoleUtf8Scope& operator=(const ConsoleUtf8Scope&) = delete;

#ifdef _WIN32
private:
    unsigned input_cp_ = 0;  // 0: geri yüklenecek bir şey yok
    unsigned output_cp_ = 0;
    bool vt_changed_ = false;        // stdout konsol kipini biz değiştirdik mi
    unsigned long stdout_mode_ = 0;  // değiştirmeden önceki kip (DWORD)
#endif
};

}  // namespace noteapp::core
