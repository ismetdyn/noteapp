#pragma once

#include <cstddef>
#include <cstdint>

// Yalnız pdf_engine.cpp ve testleri içindir; dışa açık API'ye (pdf_engine.hpp) girmez.
namespace noteapp::pdfview::detail {

// ---- Bellek bütçesi (limits.pdf.context_memory) ----
// Belgenin MuPDF bağlamı tüm ayırmalarını bu ayırıcıdan yapar (fz_alloc_context). Bütçe dolunca NULL döner: MuPDF
// önce kendi önbelleğini (fz_store) boşaltıp yeniden dener, olmazsa FZ_ERROR_SYSTEM atar. Sayaç ayrı kilit istemez:
// MuPDF ayırıcıyı her zaman FZ_LOCK_ALLOC kilidi altında çağırır (memory.c do_scavenging_malloc, fz_free).
struct MemoryBudget {
    std::size_t limit = SIZE_MAX;  // SIZE_MAX: sınırsız
    std::size_t used = 0;
    bool refused = false;  // bu işlemde (open, page_text, render_page) bütçe bir ayırmayı reddetti mi
};

// Her ayırmanın önündeki boy başlığı (max_align_t kadar: döndürülen adres malloc'unki kadar hizalı kalır). Bütçe
// başlığı da sayar: küçük nesnesi bol PDF'te başlık gerçek belleğin büyük payıdır.
inline constexpr std::size_t kBudgetHeaderBytes = alignof(std::max_align_t);

// fz_alloc_context imzaları; `user` bir MemoryBudget'tır. Bütçeye istenen boy + kBudgetHeaderBytes yazılır.
void* budget_malloc(void* user, std::size_t size) noexcept;
void* budget_realloc(void* user, void* old, std::size_t size) noexcept;
void budget_free(void* user, void* pointer) noexcept;

// MuPDF hatası bellek bütçesi yüzünden mi? Ancak hata FZ_ERROR_SYSTEM (ayırma başarısız) ve bütçe AYNI işlemde bir
// ayırmayı reddetmişse. Bütçe reddi tek başına yetmez: MuPDF bazı reddedilen ayırmaları sessizce atlar (önbellek,
// no_throw ayırma) ve işlem ardından başka nedenle (bozuk nesne) düşebilir; o hata bozuk belge sayılır.
bool is_budget_failure(int mupdf_error, bool refused_this_operation) noexcept;

}  // namespace noteapp::pdfview::detail
