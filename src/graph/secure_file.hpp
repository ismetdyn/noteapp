#pragma once

// İç başlık. Git'ten gelen dizinde dosya açmak: yol saldırgana ait olabilir. Bağ izlenmez, geçici dosya
// tahmin edilemez ada özel kurulur, yalnız normal dosya okunur (CWE-59). Hata nedeni kararlı metindir, rapora
// gider.
#include "graph/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace noteapp::graph::detail {

// POSIX'te int, Windows'ta HANDLE; başlıkta platform türü görünmez.
using NativeFile = std::intptr_t;

class InputFile {
public:
    // Dosya yoksa nullopt. Sembolik bağ "symlink_refused", normal olmayan "not_regular_file" hatasıdır.
    static Result<std::optional<InputFile>> open(const std::filesystem::path& path);

    InputFile(InputFile&& other) noexcept;
    InputFile& operator=(InputFile&& other) noexcept;
    ~InputFile();

    std::uint64_t size() const { return size_; }
    // Tam okur; dosya beklenenden kısaysa hata.
    Result<void> read_at(std::uint64_t offset, std::span<std::byte> out);

private:
    InputFile(NativeFile file, std::uint64_t size) : file_(file), size_(size) {}
    NativeFile file_;
    std::uint64_t size_;
};

class OutputFile {
public:
    // Yol zaten varsa (bağ dahil) hata: içerik başkasına yazılamaz.
    static Result<OutputFile> create_new(const std::filesystem::path& path);
    // Var olan normal dosyanın sonuna ekler; bağı izlemez.
    static Result<OutputFile> open_append(const std::filesystem::path& path);

    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    ~OutputFile();

    Result<void> write(std::span<const std::byte> bytes);
    Result<void> close();

private:
    explicit OutputFile(NativeFile file) : file_(file) {}
    NativeFile file_;
};

// Son bileşen sembolik bağ mı. Yol yoksa false.
bool is_symbolic_link(const std::filesystem::path& path);

}  // namespace noteapp::graph::detail
