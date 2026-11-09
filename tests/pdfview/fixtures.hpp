#pragma once

// PDF testlerinin fixture'ları: depoda ikili dosya yok. Düz ASCII/şifreli belgeler libharu ile, Türkçe metin
// uygulamanın PDF dışa aktarmasıyla (gömülü DejaVu + ToUnicode), özel nesneli belgeler (Form XObject çoğalması,
// düşmanca ToUnicode, Flate/JPX görsel, onarım ve bağlantı hedefi döngüleri) aşağıdaki küçük PDF yazıcısıyla üretilir.
#include "core/error.hpp"
#include "core/limits.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::pdftest {

// Sınırsız: her alan Limit{0, ""}.
core::PdfLimits unlimited();
core::Limit limit_of(std::uint64_t value);

struct Passwords {
    const char* owner = nullptr;
    const char* user = nullptr;
};

// `pages` sayfalık PDF; sayfa i'de "Page i" (1'den) yazar. Parola verilirse RC4-128 ile şifrelenir. 200x100 pt.
std::string ascii_pdf(int pages, Passwords passwords = {});
// Uygulamanın PDF dışa aktarmasıyla (gömülü DejaVu, ToUnicode) tek sayfa düz metin.
std::string exported_pdf(std::string_view text);
// Aynı yolla markdown: başlıklar büyük ve kalın, **kalın**, *italik*, `kod` biçimli yazı tipleriyle.
std::string exported_markdown_pdf(std::string_view markdown);

// ---- Küçük PDF yazıcısı: nesneleri numaralar, doğru xref tablosu yazar ----

class PdfBuilder {
public:
    int reserve() {
        objects_.emplace_back();
        return static_cast<int>(objects_.size());
    }
    int add(std::string body) {
        objects_.push_back(std::move(body));
        return static_cast<int>(objects_.size());
    }
    void set(int number, std::string body) { objects_[static_cast<std::size_t>(number - 1)] = std::move(body); }

    std::string build(int root) const {
        std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            offsets.push_back(out.size());
            out += std::format("{} 0 obj\n{}\nendobj\n", i + 1, objects_[i]);
        }
        const std::size_t xref = out.size();
        out += std::format("xref\n0 {}\n0000000000 65535 f \n", objects_.size() + 1);
        for (const std::size_t offset : offsets)
            out += std::format("{:010} 00000 n \n", offset);  // her satır 20 bayt
        out += std::format("trailer\n<< /Size {} /Root {} 0 R >>\nstartxref\n{}\n%%EOF\n", objects_.size() + 1, root,
                           xref);
        return out;
    }

private:
    std::vector<std::string> objects_;
};

std::string ref(int number);
std::string stream_object(std::string_view dictionary_entries, std::string_view data);
std::string flate(std::string_view data);

struct PageSpec {
    std::string media_box = "[0 0 100 100]";
    std::string resources = "<< >>";
    std::string content;
    bool flate_content = false;  // içerik akışı FlateDecode (ad "compress" değil: zlib.h Z_PREFIX makrosu)
};

constexpr std::string_view kHelvetica = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>";

std::string finish_pdf(PdfBuilder& builder, const std::vector<PageSpec>& pages);

// 10^9 yapraklı Form XObject çoğalması; `plain_second_page`: ardından "calm" yazan sıradan sayfa.
std::string fan_out_pdf(bool plain_second_page = false);
std::string hostile_to_unicode_pdf();    // ToUnicode: vekil, ESC, NUL, C1, DEL
std::string repair_spin_pdf();           // xref'siz, /N = INT_MAX nesne akışı (onarım döngüsü)
std::string link_lookup_pdf();           // bağlantı hedefi yavaş sayfa araması (L x D x K)
constexpr int kBigImageSide = 2000;  // 2000 x 2000 RGB = 12 MB çözülmüş, Flate'le ~12 KB
std::string big_image_pdf();             // sayfa 1: 2000x2000 Flate görsel, sayfa 2: "ok"
std::string huge_string_pdf();           // içerik akışında 24 MiB dizge (sözcük tamponu realloc ile büyür)
std::string jpx_pdf();                   // 16x16 JPEG 2000 görsel

bool contains(std::string_view haystack, std::string_view needle);
bool has_arg(const core::Error& error, std::string_view name, std::string_view value);

}  // namespace noteapp::pdftest
