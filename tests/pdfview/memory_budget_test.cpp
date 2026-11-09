// PDF1: MuPDF bağlamının bellek bütçesi (limits.pdf.context_memory) ve bütçe hatasının sınıflanması.
#include "pdfview/engine/memory_budget.hpp"

#include <mupdf/fitz.h>

#include <cstring>
#include <doctest/doctest.h>

using namespace noteapp::pdfview::detail;

namespace {

constexpr std::size_t kHeader = kBudgetHeaderBytes;

}  // namespace

TEST_CASE("budget_malloc charges the size plus its header, refuses over the budget and records the refusal") {
    MemoryBudget budget{.limit = 100 + 2 * kHeader, .used = 0, .refused = false};
    void* fits = budget_malloc(&budget, 60);
    REQUIRE(fits != nullptr);
    CHECK(budget.used == 60 + kHeader);
    CHECK_FALSE(budget.refused);

    REQUIRE(budget_malloc(&budget, 41) == nullptr);  // 60 + 41 + 2 başlık > 100 + 2 başlık
    CHECK(budget.refused);
    CHECK(budget.used == 60 + kHeader);

    void* exact = budget_malloc(&budget, 40);  // sınır dahil
    REQUIRE(exact != nullptr);
    CHECK(budget.used == 100 + 2 * kHeader);
    budget_free(&budget, exact);
    budget_free(&budget, fits);
    CHECK(budget.used == 0);
}

TEST_CASE("many small allocations are charged their headers, so they cannot exceed the budget in real memory") {
    constexpr std::size_t kCount = 100;
    MemoryBudget budget{.limit = kCount * (1 + kHeader), .used = 0, .refused = false};
    void* blocks[kCount] = {};
    for (auto& block : blocks) {
        block = budget_malloc(&budget, 1);
        REQUIRE(block != nullptr);
    }
    CHECK(budget.used == budget.limit);
    REQUIRE(budget_malloc(&budget, 1) == nullptr);  // başlık sayılmasaydı 100 x başlık daha sığardı
    CHECK(budget.refused);
    for (void* block : blocks)
        budget_free(&budget, block);
    CHECK(budget.used == 0);
}

TEST_CASE("budget_realloc refuses growth over the budget, keeps the old block and allows shrinking") {
    MemoryBudget budget{.limit = 100 + kHeader, .used = 0, .refused = false};
    auto* block = static_cast<unsigned char*>(budget_malloc(&budget, 50));
    REQUIRE(block != nullptr);
    std::memset(block, 0xAB, 50);

    // REQUIRE: ret edilmezse blok taşınmış olur, aşağıdaki adımlar geçersiz bloğa dokunurdu.
    REQUIRE(budget_realloc(&budget, block, 151) == nullptr);  // +101 > kalan 50
    CHECK(budget.refused);
    CHECK(budget.used == 50 + kHeader);
    CHECK(block[49] == 0xAB);  // eski blok yerinde

    budget.refused = false;
    auto* grown = static_cast<unsigned char*>(budget_realloc(&budget, block, 100));  // +50, sınır dahil
    REQUIRE(grown != nullptr);
    CHECK_FALSE(budget.refused);
    CHECK(budget.used == 100 + kHeader);
    CHECK(grown[49] == 0xAB);

    auto* shrunk = static_cast<unsigned char*>(budget_realloc(&budget, grown, 10));
    REQUIRE(shrunk != nullptr);
    CHECK(budget.used == 10 + kHeader);

    void* fresh = budget_realloc(&budget, nullptr, 20);  // NULL: malloc gibi
    REQUIRE(fresh != nullptr);
    CHECK(budget.used == 30 + 2 * kHeader);
    budget_free(&budget, fresh);
    budget_free(&budget, shrunk);
    budget_free(&budget, nullptr);
    CHECK(budget.used == 0);
}

TEST_CASE("an over-committed budget refuses everything instead of wrapping around") {
    MemoryBudget budget{.limit = 10, .used = 20, .refused = false};  // bağlam kurulumu sınırı aşmış
    REQUIRE(budget_malloc(&budget, 1) == nullptr);
    CHECK(budget.refused);
}

TEST_CASE("a MuPDF failure counts as the memory budget only for a system error with a refusal in that operation") {
    CHECK(is_budget_failure(FZ_ERROR_SYSTEM, true));
    CHECK_FALSE(is_budget_failure(FZ_ERROR_SYSTEM, false));  // gerçek sistem belleği ya da başka G/Ç
    CHECK_FALSE(is_budget_failure(FZ_ERROR_FORMAT, true));   // reddedilen ayırma atlandı, belge bozuk
    CHECK_FALSE(is_budget_failure(FZ_ERROR_SYNTAX, true));
    CHECK_FALSE(is_budget_failure(FZ_ERROR_ARGUMENT, true));
}
