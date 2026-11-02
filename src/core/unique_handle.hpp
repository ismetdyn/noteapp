#pragma once

#ifdef _WIN32
#include <memory>
#include <windows.h>
#else
#include <unistd.h>
#include <utility>
#endif

// İşletim sistemi tutamaçlarının tek sahibi. Yalnız bu türleri zaten kullanan .cpp'ler içerir: Windows ve
// POSIX başlıkları genel arayüzlere sızmasın.
namespace noteapp::core {

#ifdef _WIN32

template <auto Close> struct WinClose {
    void operator()(HANDLE handle) const noexcept { Close(handle); }
};
using UniqueHandle = std::unique_ptr<void, WinClose<CloseHandle>>;
using UniqueFind = std::unique_ptr<void, WinClose<FindClose>>;

// CreateFileW ve FindFirstFileExW hatada nullptr değil INVALID_HANDLE_VALUE döner: boş sahibe çevrilir.
template <class Owner> Owner own_win_handle(HANDLE handle) noexcept {
    return Owner(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

#else

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    explicit operator bool() const noexcept { return fd_ >= 0; }
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset() noexcept {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

#endif

}  // namespace noteapp::core
