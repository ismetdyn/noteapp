#pragma once

namespace noteapp::core {

// Windows'ta konsolun giriş/çıkış kod sayfasını UTF-8'e çeker, yıkıcıda eskisini geri yükler (kullanıcının
// konsolu, ör. cmd'de 857, değişmiş kalmasın). POSIX'te ve konsola bağlı olmayan süreçte etkisizdir.
class ConsoleUtf8Scope {
public:
    ConsoleUtf8Scope();
    ~ConsoleUtf8Scope();
    ConsoleUtf8Scope(const ConsoleUtf8Scope&) = delete;
    ConsoleUtf8Scope& operator=(const ConsoleUtf8Scope&) = delete;

private:
    unsigned input_cp_ = 0;  // 0: geri yüklenecek bir şey yok
    unsigned output_cp_ = 0;
};

}  // namespace noteapp::core
