#pragma once

// Sahte PDF işçisinin (fake_pdf_worker.cpp) kipi. İşçi boş ortamla başlar (PDF-SBX, ChildProcess::start_confined):
// kip ortamdan değil, ikilinin yanındaki `mode` dosyasından okunur. Her örnek ikilinin kopyasını kendi geçici
// dizininde tutar (paralel test süreçleri birbirinin kipini görmez); kip belge açıkken de değiştirilebilir, yeniden
// başlatılan işçi yeni kipi okur.
#include "support/test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace noteapp::test {

class FakePdfWorker {
public:
    explicit FakePdfWorker(const std::filesystem::path& binary) : path_(dir_.path() / binary.filename()) {
        std::filesystem::copy_file(binary, path_);
    }

    void set_mode(std::string_view mode) const {
        std::ofstream(dir_.path() / "mode", std::ios::binary | std::ios::trunc) << mode;
    }
    const std::filesystem::path& path() const { return path_; }

private:
    TempDir dir_;
    std::filesystem::path path_;
};

}  // namespace noteapp::test
