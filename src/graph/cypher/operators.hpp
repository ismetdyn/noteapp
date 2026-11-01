#pragma once

// Planın düğümleri: graph işleçleri. Satır yerinde değişir, her işleç yalnız kendi yuvalarına yazar; bu
// yüzden gezinme satır kopyalamaz. Depolama yalnız ReadTxn/WriteTxn üzerinden görülür.
#include "graph/cypher/ast.hpp"
#include "graph/cypher/runtime.hpp"
#include "graph/error.hpp"
#include "graph/graph.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::graph::detail::cypher {

class Operator {
public:
    explicit Operator(std::string name) : name_(std::move(name)) {}
    virtual ~Operator() = default;
    Operator(const Operator&) = delete;
    Operator& operator=(const Operator&) = delete;

    // true: `row` yeni eşleşmeyi taşır. false: bitti; yaprak yeniden kurulduysa tekrar çağrılabilir.
    virtual Result<bool> next(Row& row, ExecContext& context) = 0;
    virtual void describe(std::vector<std::string>& out) const { out.push_back(name_); }

protected:
    std::string name_;
};

using OperatorPtr = std::unique_ptr<Operator>;

// Yaprak: bir kez boş girdi satırı verir. OptionalApply ve Merge iç planını satır başına yeniden kurar.
class OnceOp final : public Operator {
public:
    OnceOp() : Operator("Once") {}
    Result<bool> next(Row& row, ExecContext& context) override;
    void rearm() { pending_ = true; }

private:
    bool pending_ = true;
};

class PipelineOp : public Operator {
public:
    PipelineOp(std::string name, OperatorPtr child) : Operator(std::move(name)), child_(std::move(child)) {}
    void describe(std::vector<std::string>& out) const override {
        child_->describe(out);
        Operator::describe(out);
    }

protected:
    OperatorPtr child_;
};

struct ExpandSpec {
    std::string name;
    int src = -1;
    int dst = -1;
    int edge = -1;  // Expand: kenar yuvası; VarLength: yol listesi yuvası veya -1
    bool dst_bound = false;
    Direction direction = Direction::Out;
    const TypeSet* types = nullptr;
    int min_hops = 1;
    int max_hops = 1;
    Position pos;
    // Aynı MATCH'te önceden bağlanan kenarlar ve değişken uzunluklu yollar: kenar benzersizliği.
    std::vector<int> earlier_edges;
    std::vector<int> earlier_paths;
};

struct ProjectItem {
    const Expr* expr;
    int slot;
};

struct SortKey {
    const Expr* expr;
    bool descending;
};

struct AggregateSpec {
    AggregateFn fn;
    bool distinct;
    const Expr* arg;  // count(*)'da yok
    int slot;
    Position pos;
};

OperatorPtr make_node_scan(OperatorPtr child, std::string name, int slot);
OperatorPtr make_label_scan(OperatorPtr child, std::string name, int slot, std::string label);
OperatorPtr make_id_seek(OperatorPtr child, std::string name, int slot, const Expr* id);
// Bağlı değişken düğüm olmalı: null satırı düşürür, başka tip hatadır.
OperatorPtr make_check_node(OperatorPtr child, int slot, Position pos);
OperatorPtr make_expand(OperatorPtr child, ExpandSpec spec);
OperatorPtr make_var_length_expand(OperatorPtr child, ExpandSpec spec);
OperatorPtr make_filter(OperatorPtr child, const Expr* predicate);
OperatorPtr make_project(OperatorPtr child, std::vector<ProjectItem> items);
OperatorPtr make_distinct(OperatorPtr child, std::vector<int> slots);
// `child_writes`: alt ağaçta yazma var. Sıfır satır istense de bir kez sürülür: yazma işleçleri ilk çağrıda
// hepsini yazar, LIMIT 0 yazmayı atlatamaz.
OperatorPtr make_sort(OperatorPtr child, std::vector<SortKey> keys, const Expr* skip, const Expr* limit,
                      bool child_writes);
OperatorPtr make_skip(OperatorPtr child, const Expr* count);
OperatorPtr make_limit(OperatorPtr child, const Expr* count, bool child_writes);
OperatorPtr make_aggregate(OperatorPtr child, std::vector<ProjectItem> keys,
                           std::vector<AggregateSpec> aggregates, std::vector<ProjectItem> finals);
OperatorPtr make_optional_apply(OperatorPtr child, OperatorPtr inner, OnceOp* inner_leaf,
                                std::vector<int> null_slots);

// Yazma işleçleri (write_operators.cpp). Girdiyi önce tümüyle materyalize eder ve hepsini yazar: tarama
// yazmayla çakışmaz, LIMIT yazmayı kısmaz.
struct WriteProperty {
    std::string key;
    const Expr* value;
};

struct WriteNode {
    int slot = -1;
    bool bound = false;
    Position pos;
    std::vector<std::string> labels;
    std::vector<WriteProperty> props;
};

struct WriteEdge {
    int slot = -1;
    int src = -1;
    int dst = -1;
    Position pos;
    std::string type;
    std::vector<WriteProperty> props;
};

struct WriteStep {
    std::variant<WriteNode, WriteEdge> what;
};

struct SetOperation {
    const Expr* target;
    Position pos;
    std::string key;                  // boşsa etiket işlemi
    const Expr* value = nullptr;      // özellik; yoksa kaldırma
    std::vector<std::string> labels;  // etiket
};

OperatorPtr make_create(OperatorPtr child, std::vector<WriteStep> steps);
OperatorPtr make_merge(OperatorPtr child, OperatorPtr inner, OnceOp* inner_leaf,
                       std::vector<WriteStep> steps);
OperatorPtr make_set(OperatorPtr child, std::string name, bool remove, std::vector<SetOperation> operations);
OperatorPtr make_delete(OperatorPtr child, bool detach, std::vector<const Expr*> targets);

}  // namespace noteapp::graph::detail::cypher
