#include "pdfview/engine/memory_budget.hpp"

#include <cstdlib>
#include <cstring>

#include <mupdf/fitz.h>

namespace noteapp::pdfview::detail {

namespace {

// `extra` bayt daha bütçeye sığar mı. `used > limit` yalnız bağlam kurulumunda olabilir (open bunu reddeder);
// yine de çıkarma taşmasın.
bool fits(const MemoryBudget& budget, std::size_t extra) noexcept {
    return budget.used <= budget.limit && extra <= budget.limit - budget.used;
}

static_assert(kBudgetHeaderBytes >= sizeof(std::size_t));

// Başlıkta bloğun bütçeye yazılan boyu (istenen + başlık) durur: free ve realloc aynı sayıyı geri alır.
std::size_t stored_charge(const unsigned char* block) noexcept {
    std::size_t charge = 0;
    std::memcpy(&charge, block, sizeof charge);
    return charge;
}

}  // namespace

void* budget_malloc(void* user, std::size_t size) noexcept {
    auto& budget = *static_cast<MemoryBudget*>(user);
    if (size > SIZE_MAX - kBudgetHeaderBytes)
        return nullptr;
    const std::size_t charge = size + kBudgetHeaderBytes;
    if (!fits(budget, charge)) {
        budget.refused = true;
        return nullptr;
    }
    auto* block = static_cast<unsigned char*>(std::malloc(charge));
    if (block == nullptr)
        return nullptr;
    std::memcpy(block, &charge, sizeof charge);
    budget.used += charge;
    return block + kBudgetHeaderBytes;
}

void budget_free(void* user, void* pointer) noexcept {
    if (pointer == nullptr)
        return;
    auto& budget = *static_cast<MemoryBudget*>(user);
    unsigned char* block = static_cast<unsigned char*>(pointer) - kBudgetHeaderBytes;
    budget.used -= stored_charge(block);
    std::free(block);
}

void* budget_realloc(void* user, void* old, std::size_t size) noexcept {
    if (old == nullptr)
        return budget_malloc(user, size);
    auto& budget = *static_cast<MemoryBudget*>(user);
    if (size > SIZE_MAX - kBudgetHeaderBytes)
        return nullptr;
    unsigned char* block = static_cast<unsigned char*>(old) - kBudgetHeaderBytes;
    const std::size_t old_charge = stored_charge(block);
    const std::size_t charge = size + kBudgetHeaderBytes;
    if (charge > old_charge && !fits(budget, charge - old_charge)) {
        budget.refused = true;
        return nullptr;
    }
    auto* grown = static_cast<unsigned char*>(std::realloc(block, charge));
    if (grown == nullptr)
        return nullptr;  // eski blok yerinde ve hâlâ sayılı
    std::memcpy(grown, &charge, sizeof charge);
    budget.used = budget.used - old_charge + charge;
    return grown + kBudgetHeaderBytes;
}

bool is_budget_failure(int mupdf_error, bool refused_this_operation) noexcept {
    return mupdf_error == FZ_ERROR_SYSTEM && refused_this_operation;
}

}  // namespace noteapp::pdfview::detail
