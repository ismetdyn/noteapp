#include "graph/cypher/planner.hpp"

#include "graph/cypher/expr.hpp"
#include "graph/cypher/parser.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::graph::detail::cypher {

namespace {
// Geçerli 64 KiB sorgu 64 bin düğümün altındadır. Diğer ad açılımı bunu çarpabildiğinden plan aşamasının da
// bütçesi var.
constexpr std::size_t kMaxPlanExprNodes = 200'000;
}  // namespace

Plan::Plan() = default;
Plan::Plan(Plan&&) noexcept = default;
Plan& Plan::operator=(Plan&&) noexcept = default;
Plan::~Plan() = default;

namespace {

using Scope = std::map<std::string, int, std::less<>>;

enum class VarKind { Value, Node, Edge, Path };

ExprPtr clone_expr(const Expr& expr) {
    auto copy = std::make_unique<Expr>();
    copy->kind = expr.kind;
    copy->pos = expr.pos;
    copy->height = expr.height;
    copy->literal = expr.literal;
    copy->name = expr.name;
    copy->slot = expr.slot;
    copy->key_id = expr.key_id;
    copy->op = expr.op;
    copy->fn = expr.fn;
    copy->aggregate = expr.aggregate;
    copy->distinct = expr.distinct;
    copy->labels = expr.labels;
    for (const ExprPtr& arg : expr.args)
        copy->args.push_back(clone_expr(*arg));
    return copy;
}

std::size_t expr_nodes(const Expr& expr) {
    std::size_t nodes = 1;
    for (const ExprPtr& arg : expr.args)
        nodes += expr_nodes(*arg);
    return nodes;
}

bool has_non_finite(const QueryValue& value) {
    if (const double* number = std::get_if<double>(&value.v))
        return !std::isfinite(*number);
    if (const auto* list = std::get_if<QueryList>(&value.v))
        return std::ranges::any_of(list->items, has_non_finite);
    return false;
}

bool same_expr(const Expr& left, const Expr& right) {
    if (left.kind != right.kind || left.name != right.name || left.op != right.op || left.fn != right.fn ||
        left.aggregate != right.aggregate || left.distinct != right.distinct || left.labels != right.labels ||
        left.args.size() != right.args.size() || !(left.literal == right.literal))
        return false;
    for (std::size_t i = 0; i < left.args.size(); ++i)
        if (!same_expr(*left.args[i], *right.args[i]))
            return false;
    return true;
}

bool has_aggregate(const Expr& expr) {
    if (expr.kind == Expr::Kind::Aggregate)
        return true;
    return std::ranges::any_of(expr.args, [](const ExprPtr& arg) { return has_aggregate(*arg); });
}

void collect_dependencies(const Expr& expr, std::vector<int>& out) {
    if (expr.kind == Expr::Kind::Var)
        out.push_back(expr.slot);
    for (const ExprPtr& arg : expr.args)
        collect_dependencies(*arg, out);
}

// Mevcut bağlanmamış değişkenin başlangıç tercihi (tasks/graph-design.md "Planner"), büyük olan kazanır.
enum StartScore { kScanAll = 0, kLabel = 1, kLabelAndEquality = 2, kIdSeek = 3, kBound = 4 };

// WHERE'in AND'den bölünmüş parçası; değişkenleri bağlanınca en erken işlece iner.
struct PendingFilter {
    const Expr* expr;
    std::vector<int> dependencies;
    bool applied = false;
};

// Bir MATCH'in planlanma durumu: kenar benzersizliği cümle içinde geçerlidir.
struct MatchState {
    std::vector<PendingFilter> filters;
    std::vector<int> used_edges;
    std::vector<int> used_paths;
    std::size_t planned_rels = 0;
    std::size_t total_rels = 0;
};

// İlk hata saklanır, kalan cümleler planlanmaz: işleçler hiç çalışmaz, yuvasız ifade değerlendirilmez.
class Planner {
public:
    Planner(Plan& plan, const Params& params, const QueryLimits& limits)
        : plan_(plan), params_(params), limits_(limits) {}

    Result<void> run() {
        plan_.root = std::make_unique<OnceOp>();
        for (Clause& clause : plan_.query.clauses) {
            std::visit([this](auto& value) { plan_clause(value); }, clause);
            if (error_)
                return std::unexpected(std::move(*error_));
        }
        plan_.slot_count = static_cast<int>(bound_.size());
        return {};
    }

private:
    // --- Yardımcılar ---

    void syntax(std::string what, Position pos) {
        if (!error_)
            error_ =
                located_error(Error::Code::Syntax, "graph.cypher_syntax", pos, {{"what", std::move(what)}});
    }
    void unsupported(std::string what, Position pos) {
        if (!error_)
            error_ = located_error(Error::Code::Unsupported, "graph.cypher_unsupported", pos,
                                   {{"what", std::move(what)}});
    }

    int new_slot(VarKind kind = VarKind::Value) {
        bound_.push_back(false);
        kinds_.push_back(kind);
        return static_cast<int>(bound_.size()) - 1;
    }

    bool is_bound(int slot) const { return bound_[static_cast<std::size_t>(slot)]; }
    void mark_bound(int slot) { bound_[static_cast<std::size_t>(slot)] = true; }
    VarKind kind_of(int slot) const { return kinds_[static_cast<std::size_t>(slot)]; }

    int key_id_of(const std::string& key) {
        return key_ids_.try_emplace(key, static_cast<int>(key_ids_.size())).first->second;
    }

    const TypeSet* type_set_of(std::vector<std::string> types) {
        std::ranges::sort(types);
        types.erase(std::ranges::unique(types).begin(), types.end());
        for (const TypeSet& known : plan_.type_sets)
            if (known.types == types)
                return &known;
        plan_.type_sets.push_back(
            TypeSet{static_cast<std::uint32_t>(plan_.type_sets.size()), std::move(types)});
        return &plan_.type_sets.back();
    }

    const Expr* own(ExprPtr expr) {
        plan_.owned_exprs.push_back(std::move(expr));
        return plan_.owned_exprs.back().get();
    }

    static ExprPtr var_at(int slot, Position pos) {
        auto var = std::make_unique<Expr>();
        var->kind = Expr::Kind::Var;
        var->pos = pos;
        var->slot = slot;
        return var;
    }

    ExprPtr property_at(int slot, const std::string& key, Position pos) {
        auto property = std::make_unique<Expr>();
        property->kind = Expr::Kind::Property;
        property->pos = pos;
        property->slot = slot;
        property->name = key;
        property->key_id = key_id_of(key);
        property->args.push_back(var_at(slot, pos));
        return property;
    }

    static ExprPtr equality(ExprPtr left, ExprPtr right, Position pos) {
        auto expr = std::make_unique<Expr>();
        expr->kind = Expr::Kind::Binary;
        expr->op = BinaryOp::Eq;
        expr->pos = pos;
        expr->args.push_back(std::move(left));
        expr->args.push_back(std::move(right));
        return expr;
    }

    // --- Çözümleme: ad -> yuva, parametre -> sabit. Depoya dokunmaz. ---

    void resolve_param(Expr& expr) {
        const auto found = params_.find(expr.name);
        if (found == params_.end()) {
            if (!error_)
                error_ = located_error(Error::Code::InvalidArgument, "graph.cypher_missing_param", expr.pos,
                                       {{"name", expr.name}});
            return;
        }
        // Sonsuz ve NaN sorguya girmez: aritmetik ve depo da üretmez.
        if (has_non_finite(found->second)) {
            if (!error_)
                error_ = located_error(Error::Code::InvalidArgument, "graph.invalid_argument", expr.pos,
                                       {{"reason", "non_finite"}});
            return;
        }
        if (value_depth(found->second) >= kMaxValueDepth && !error_)
            error_ = limit_error("value_depth");
        // Her kullanım değeri kopyalar: `[$b, $b, ...]` plan aşamasında belleği çarpmasın.
        param_bytes_ += value_bytes(found->second);
        if (param_bytes_ > limits_.max_memory_bytes) {
            if (!error_)
                error_ = limit_error("memory");
            return;
        }
        expr.kind = Expr::Kind::Literal;
        expr.literal = found->second;
    }

    void resolve_var(Expr& expr, const Scope& scope) {
        if (expr.slot >= 0)
            return;  // planlayıcının kurduğu veya daha önce çözülmüş
        const auto found = scope.find(expr.name);
        if (found == scope.end()) {
            syntax("undefined variable: " + expr.name, expr.pos);
            return;
        }
        expr.slot = found->second;
    }

    void resolve(Expr& expr, const Scope& scope) {
        switch (expr.kind) {
        case Expr::Kind::Param:
            resolve_param(expr);
            return;
        case Expr::Kind::Var:
            resolve_var(expr, scope);
            return;
        case Expr::Kind::Aggregate:
            syntax("aggregate is not allowed here", expr.pos);
            return;
        default:
            break;
        }
        for (ExprPtr& arg : expr.args)
            resolve(*arg, scope);
        // Hedef değişken değilse yuva yok, hedef çalışma anında değerlendirilir (açılmış diğer ad).
        if (expr.kind == Expr::Kind::Property || expr.kind == Expr::Kind::HasLabels)
            expr.slot = expr.args[0]->kind == Expr::Kind::Var ? expr.args[0]->slot : -1;
        if (expr.kind == Expr::Kind::Property)
            expr.key_id = key_id_of(expr.name);
    }

    // --- Cümleler ---

    void plan_clause(MatchClause& clause) {
        OperatorPtr outer;
        OnceOp* leaf = nullptr;
        if (clause.optional) {
            outer = std::move(plan_.root);
            auto once = std::make_unique<OnceOp>();
            leaf = once.get();
            plan_.root = std::move(once);
        }
        const std::size_t slots_before = bound_.size();
        plan_pattern(clause.paths, clause.where);
        if (clause.optional) {
            std::vector<int> new_slots;
            for (std::size_t slot = slots_before; slot < bound_.size(); ++slot)
                new_slots.push_back(static_cast<int>(slot));
            plan_.root =
                make_optional_apply(std::move(outer), std::move(plan_.root), leaf, std::move(new_slots));
        }
    }

    void plan_clause(CreateClause& clause) {
        record_first_write(clause.pos);
        std::vector<WriteStep> steps;
        for (PathPattern& path : clause.paths)
            plan_create_path(path, steps);
        if (!error_)
            plan_.root = make_create(std::move(plan_.root), std::move(steps));
    }

    void plan_clause(MergeClause& clause) {
        record_first_write(clause.pos);
        // Desenin değerleri dış kapsamda çözülür: MERGE kendi değişkenine bakarak kimlik türetemez.
        for (NodePattern& node : clause.path.nodes)
            resolve_properties(node.props, scope_);
        for (RelPattern& rel : clause.path.rels)
            resolve_properties(rel.props, scope_);
        check_merge_pattern(clause.path);
        if (error_)
            return;
        const std::set<std::string, std::less<>> visible = names_in_scope();
        OperatorPtr outer = std::move(plan_.root);
        auto once = std::make_unique<OnceOp>();
        OnceOp* leaf = once.get();
        plan_.root = std::move(once);
        std::vector<PathPattern*> paths = {&clause.path};
        plan_pattern_paths(paths, nullptr);
        OperatorPtr inner = std::move(plan_.root);
        std::vector<WriteStep> steps = merge_steps(clause.path, visible);
        plan_.root = make_merge(std::move(outer), std::move(inner), leaf, std::move(steps));
    }

    void plan_clause(SetClause& clause) {
        record_first_write(clause.pos);
        std::vector<SetOperation> operations;
        for (SetItem& item : clause.items) {
            ExprPtr target = std::make_unique<Expr>();
            target->kind = Expr::Kind::Var;
            target->name = item.var;
            target->pos = item.pos;
            resolve(*target, scope_);
            if (item.value)
                resolve(*item.value, scope_);
            operations.push_back(
                SetOperation{own(std::move(target)), item.pos, item.key, item.value.get(), item.labels});
        }
        plan_.root = make_set(std::move(plan_.root), clause.remove ? "Remove" : "Set", clause.remove,
                              std::move(operations));
    }

    void plan_clause(DeleteClause& clause) {
        record_first_write(clause.pos);
        std::vector<const Expr*> targets;
        for (ExprPtr& target : clause.targets) {
            resolve(*target, scope_);
            targets.push_back(target.get());
        }
        plan_.root = make_delete(std::move(plan_.root), clause.detach, std::move(targets));
    }

    void plan_clause(ProjectionClause& clause) {
        expand_star(clause);
        name_items(clause);
        if (error_)
            return;
        const bool aggregated = std::ranges::any_of(
            clause.items, [](const ProjectionItem& item) { return has_aggregate(*item.expr); });
        const std::vector<int> slots = aggregated        ? plan_aggregated(clause)
                                       : clause.distinct ? plan_distinct(clause)
                                                         : plan_plain(clause);
        if (error_)
            return;
        if (clause.where) {
            resolve(*clause.where, scope_);
            plan_.root = make_filter(std::move(plan_.root), clause.where.get());
        }
        if (clause.is_return) {
            for (const ProjectionItem& item : clause.items)
                plan_.columns.push_back(item.alias);
            plan_.column_slots = slots;
        }
    }

    void record_first_write(Position pos) {
        if (!plan_.first_write)
            plan_.first_write = pos;
    }

    std::set<std::string, std::less<>> names_in_scope() const {
        std::set<std::string, std::less<>> names;
        for (const auto& [name, slot] : scope_)
            names.insert(name);
        return names;
    }

    // --- Yansıtma ---

    void expand_star(ProjectionClause& clause) {
        if (!clause.star)
            return;
        if (scope_.empty())
            syntax("* needs at least one variable in scope", clause.pos);
        std::vector<ProjectionItem> items;
        for (const auto& [name, slot] : scope_) {
            ProjectionItem item;
            item.expr = std::make_unique<Expr>();
            item.expr->kind = Expr::Kind::Var;
            item.expr->name = name;
            item.expr->pos = clause.pos;
            item.alias = name;
            item.explicit_alias = true;
            items.push_back(std::move(item));
        }
        for (ProjectionItem& item : clause.items)
            items.push_back(std::move(item));
        clause.items = std::move(items);
    }

    void name_items(ProjectionClause& clause) {
        std::set<std::string, std::less<>> seen;
        for (ProjectionItem& item : clause.items) {
            if (!item.explicit_alias && item.expr->kind == Expr::Kind::Var)
                item.alias = item.expr->name;
            else if (!item.explicit_alias && !clause.is_return)
                syntax("WITH expression needs an alias: use AS", item.expr->pos);
            if (!seen.insert(item.alias).second)
                syntax("duplicate column name: " + item.alias, item.expr->pos);
        }
    }

    // Sonuç yuvaları yeni kapsamdır: WITH öncesi adlar burada kaybolur.
    std::vector<int> allocate_outputs(const ProjectionClause& clause) {
        std::vector<int> slots;
        for (const ProjectionItem& item : clause.items) {
            VarKind kind = VarKind::Value;
            if (item.expr->kind == Expr::Kind::Var && item.expr->slot >= 0)
                kind = kind_of(item.expr->slot);
            const int slot = new_slot(kind);
            mark_bound(slot);
            slots.push_back(slot);
        }
        return slots;
    }

    void enter_scope(const ProjectionClause& clause, const std::vector<int>& slots) {
        Scope scope;
        for (std::size_t i = 0; i < clause.items.size(); ++i)
            scope[clause.items[i].alias] = slots[i];
        scope_ = std::move(scope);
    }

    // ORDER BY'daki diğer ad, öğenin ifadesiyle değiştirilir: sıralama yansıtmadan önce koşar, yalnız seçilen
    // satırların sütunları hesaplanır (LIMIT 20 için satır başına ad okunmaz).
    void inline_aliases(ExprPtr& expr, const ProjectionClause& clause) {
        if (expr->kind == Expr::Kind::Var) {
            for (const ProjectionItem& item : clause.items) {
                if (item.alias == expr->name) {
                    expr = budgeted_clone(*item.expr);
                    return;
                }
            }
        }
        for (ExprPtr& arg : expr->args) {
            if (error_)
                return;
            inline_aliases(arg, clause);
        }
    }

    // Kopyadan önce sayılır: `[1, …] AS a ORDER BY [a, a, …]` kopyalamadan reddedilmeli.
    ExprPtr budgeted_clone(const Expr& expr) {
        plan_nodes_ += expr_nodes(expr);
        if (plan_nodes_ > kMaxPlanExprNodes) {
            if (!error_)
                error_ = limit_error("plan_nodes");
            return std::make_unique<Expr>();
        }
        return clone_expr(expr);
    }

    void resolve_count(ExprPtr& count) {
        static const Scope kNoVariables;
        if (!count)
            return;
        if (expr_has_variables(*count)) {
            syntax("SKIP and LIMIT take constant expressions", count->pos);
            return;
        }
        resolve(*count, kNoVariables);
    }

    static bool expr_has_variables(const Expr& expr) {
        if (expr.kind == Expr::Kind::Var)
            return true;
        return std::ranges::any_of(expr.args, [](const ExprPtr& arg) { return expr_has_variables(*arg); });
    }

    void plan_order_skip_limit(ProjectionClause& clause) {
        resolve_count(clause.skip);
        resolve_count(clause.limit);
        if (error_)
            return;
        if (!clause.order.empty()) {
            std::vector<SortKey> keys;
            for (const OrderItem& item : clause.order)
                keys.push_back(SortKey{item.expr.get(), item.descending});
            plan_.root = make_sort(std::move(plan_.root), std::move(keys), clause.skip.get(),
                                   clause.limit.get(), plan_.first_write.has_value());
        }
        if (clause.skip)
            plan_.root = make_skip(std::move(plan_.root), clause.skip.get());
        if (clause.limit)
            plan_.root = make_limit(std::move(plan_.root), clause.limit.get(), plan_.first_write.has_value());
    }

    std::vector<int> plan_plain(ProjectionClause& clause) {
        for (OrderItem& item : clause.order) {
            inline_aliases(item.expr, clause);
            resolve(*item.expr, scope_);
        }
        std::vector<ProjectItem> outputs;
        for (ProjectionItem& item : clause.items)
            resolve(*item.expr, scope_);
        plan_order_skip_limit(clause);
        const std::vector<int> slots = allocate_outputs(clause);
        for (std::size_t i = 0; i < clause.items.size(); ++i)
            outputs.push_back(ProjectItem{clause.items[i].expr.get(), slots[i]});
        plan_.root = make_project(std::move(plan_.root), std::move(outputs));
        enter_scope(clause, slots);
        return slots;
    }

    // DISTINCT ve gruplamadan sonra yalnız yansıtılanlar görünür; ORDER BY'daki aynı ifade o sütuna bağlanır.
    static void replace_projected(ExprPtr& expr, const ProjectionClause& clause) {
        for (const ProjectionItem& item : clause.items) {
            if (same_expr(*expr, *item.expr)) {
                auto var = std::make_unique<Expr>();
                var->kind = Expr::Kind::Var;
                var->name = item.alias;
                var->pos = expr->pos;
                expr = std::move(var);
                return;
            }
        }
        for (ExprPtr& arg : expr->args)
            replace_projected(arg, clause);
    }

    std::vector<int> plan_distinct(ProjectionClause& clause) {
        for (OrderItem& item : clause.order)
            replace_projected(item.expr, clause);
        for (ProjectionItem& item : clause.items)
            resolve(*item.expr, scope_);
        const std::vector<int> slots = allocate_outputs(clause);
        std::vector<ProjectItem> outputs;
        for (std::size_t i = 0; i < clause.items.size(); ++i)
            outputs.push_back(ProjectItem{clause.items[i].expr.get(), slots[i]});
        plan_.root = make_project(std::move(plan_.root), std::move(outputs));
        plan_.root = make_distinct(std::move(plan_.root), slots);
        enter_scope(clause, slots);
        for (OrderItem& item : clause.order)
            resolve(*item.expr, scope_);
        plan_order_skip_limit(clause);
        return slots;
    }

    // Toplama çağrısını gizli yuvaya bağlı değişkene çevirir, argümanı dış kapsamda çözer.
    void extract_aggregates(Expr& expr, std::vector<AggregateSpec>& aggregates) {
        if (expr.kind != Expr::Kind::Aggregate) {
            for (ExprPtr& arg : expr.args)
                extract_aggregates(*arg, aggregates);
            return;
        }
        const Expr* arg = nullptr;
        if (!expr.args.empty()) {
            resolve(*expr.args[0], scope_);
            arg = own(std::move(expr.args[0]));
        }
        const int slot = new_slot();
        mark_bound(slot);
        aggregates.push_back(AggregateSpec{expr.aggregate, expr.distinct, arg, slot, expr.pos});
        expr.kind = Expr::Kind::Var;
        expr.slot = slot;
        expr.name.clear();
        expr.args.clear();
    }

    std::vector<int> plan_aggregated(ProjectionClause& clause) {
        static const Scope kOnlyAggregates;
        for (OrderItem& item : clause.order)
            replace_projected(item.expr, clause);
        std::vector<int> slots;
        std::vector<ProjectItem> keys;
        std::vector<AggregateSpec> aggregates;
        std::vector<ProjectItem> finals;
        for (ProjectionItem& item : clause.items) {
            if (!has_aggregate(*item.expr)) {
                resolve(*item.expr, scope_);
                const int slot = new_slot(item.expr->kind == Expr::Kind::Var && item.expr->slot >= 0
                                              ? kind_of(item.expr->slot)
                                              : VarKind::Value);
                mark_bound(slot);
                keys.push_back(ProjectItem{item.expr.get(), slot});
                slots.push_back(slot);
                continue;
            }
            extract_aggregates(*item.expr, aggregates);
            // Toplama dışındaki değişken gruplamayı belirsizleştirir: n.a + count(*) reddedilir.
            resolve(*item.expr, kOnlyAggregates);
            if (item.expr->kind == Expr::Kind::Var) {
                slots.push_back(item.expr->slot);
                continue;
            }
            const int slot = new_slot();
            mark_bound(slot);
            finals.push_back(ProjectItem{item.expr.get(), slot});
            slots.push_back(slot);
        }
        plan_.root =
            make_aggregate(std::move(plan_.root), std::move(keys), std::move(aggregates), std::move(finals));
        if (clause.distinct)
            plan_.root = make_distinct(std::move(plan_.root), slots);
        enter_scope(clause, slots);
        for (OrderItem& item : clause.order)
            resolve(*item.expr, scope_);
        plan_order_skip_limit(clause);
        return slots;
    }

    // --- Desen ---

    void declare_node(NodePattern& node) {
        if (node.var.empty()) {
            node.slot = new_slot(VarKind::Node);
            return;
        }
        const auto found = scope_.find(node.var);
        if (found == scope_.end()) {
            node.slot = new_slot(VarKind::Node);
            scope_[node.var] = node.slot;
            return;
        }
        node.slot = found->second;
        if (kind_of(node.slot) == VarKind::Edge || kind_of(node.slot) == VarKind::Path)
            syntax("variable is a relationship, not a node: " + node.var, node.pos);
    }

    void declare_rel(RelPattern& rel, std::set<std::string, std::less<>>& seen_in_clause) {
        const VarKind kind = rel.var_length ? VarKind::Path : VarKind::Edge;
        if (rel.var.empty()) {
            rel.slot = new_slot(kind);
            return;
        }
        if (!seen_in_clause.insert(rel.var).second)
            syntax("relationship variable used twice in one pattern: " + rel.var, rel.pos);
        if (scope_.contains(rel.var)) {
            unsupported("re-binding a relationship variable: " + rel.var, rel.pos);
            rel.slot = new_slot(kind);  // planlama hata saklanana dek sürer: yuvasız kalmasın
            return;
        }
        rel.slot = new_slot(kind);
        scope_[rel.var] = rel.slot;
    }

    void resolve_properties(PropertyMap& props, const Scope& scope) {
        for (auto& [key, value] : props)
            resolve(*value, scope);
    }

    void add_filter(MatchState& state, ExprPtr expr) {
        resolve(*expr, scope_);
        std::vector<int> dependencies;
        collect_dependencies(*expr, dependencies);
        state.filters.push_back(PendingFilter{own(std::move(expr)), std::move(dependencies)});
    }

    void split_where(MatchState& state, ExprPtr expr) {
        if (expr->kind == Expr::Kind::Binary && expr->op == BinaryOp::And) {
            split_where(state, std::move(expr->args[0]));
            split_where(state, std::move(expr->args[1]));
            return;
        }
        add_filter(state, std::move(expr));
    }

    void add_inline_filters(MatchState& state, int slot, const PropertyMap& props, Position pos) {
        for (const auto& [key, value] : props)
            add_filter(state, equality(property_at(slot, key, pos), budgeted_clone(*value), pos));
    }

    void flush_filters(MatchState& state) {
        for (PendingFilter& filter : state.filters) {
            if (filter.applied ||
                !std::ranges::all_of(filter.dependencies, [this](int slot) { return is_bound(slot); }))
                continue;
            plan_.root = make_filter(std::move(plan_.root), filter.expr);
            filter.applied = true;
        }
    }

    void plan_label_filter(const NodePattern& node, std::size_t skip_labels) {
        if (node.labels.size() <= skip_labels)
            return;
        auto expr = std::make_unique<Expr>();
        expr->kind = Expr::Kind::HasLabels;
        expr->pos = node.pos;
        expr->slot = node.slot;
        expr->labels.assign(node.labels.begin() + static_cast<std::ptrdiff_t>(skip_labels),
                            node.labels.end());
        expr->args.push_back(var_at(node.slot, node.pos));
        plan_.root = make_filter(std::move(plan_.root), own(std::move(expr)));
    }

    bool depends_only_on_bound(const Expr& expr, int excluded) const {
        std::vector<int> dependencies;
        collect_dependencies(expr, dependencies);
        return std::ranges::all_of(dependencies,
                                   [&](int slot) { return slot != excluded && is_bound(slot); });
    }

    // `id(n) = X` iletişimi: X o anda değerlendirilebiliyorsa tek okuma.
    const Expr* id_seek_value(const MatchState& state, int slot) const {
        for (const PendingFilter& filter : state.filters) {
            const Expr& expr = *filter.expr;
            if (filter.applied || expr.kind != Expr::Kind::Binary || expr.op != BinaryOp::Eq)
                continue;
            for (std::size_t side = 0; side < 2; ++side) {
                const Expr& left = *expr.args[side];
                const Expr& right = *expr.args[1 - side];
                if (left.kind == Expr::Kind::Call && left.fn == Function::Id &&
                    left.args[0]->kind == Expr::Kind::Var && left.args[0]->slot == slot &&
                    depends_only_on_bound(right, slot))
                    return &right;
            }
        }
        return nullptr;
    }

    bool has_equality(const MatchState& state, int slot) const {
        return std::ranges::any_of(state.filters, [&](const PendingFilter& filter) {
            const Expr& expr = *filter.expr;
            if (filter.applied || expr.kind != Expr::Kind::Binary || expr.op != BinaryOp::Eq)
                return false;
            const auto is_property_of_slot = [&](const Expr& side) {
                return side.kind == Expr::Kind::Property && side.slot == slot;
            };
            return (is_property_of_slot(*expr.args[0]) && depends_only_on_bound(*expr.args[1], slot)) ||
                   (is_property_of_slot(*expr.args[1]) && depends_only_on_bound(*expr.args[0], slot));
        });
    }

    int start_score(const MatchState& state, const NodePattern& node) const {
        if (is_bound(node.slot))
            return kBound;
        if (id_seek_value(state, node.slot) != nullptr)
            return kIdSeek;
        if (node.labels.empty())
            return kScanAll;
        return has_equality(state, node.slot) ? kLabelAndEquality : kLabel;
    }

    void plan_start(MatchState& state, const PathPattern& path, const NodePattern& node) {
        std::size_t scanned_labels = 0;
        if (is_bound(node.slot)) {
            // Tek düğümlü desende Expand yok: null satır burada düşer. Tipi belli olmayan değer burada
            // sınanır.
            if (path.rels.empty() || kind_of(node.slot) == VarKind::Value)
                plan_.root = make_check_node(std::move(plan_.root), node.slot, node.pos);
        } else if (const Expr* id = id_seek_value(state, node.slot)) {
            plan_.root = make_id_seek(std::move(plan_.root), "IdSeek(" + node.var + ")", node.slot, id);
        } else if (!node.labels.empty()) {
            plan_.root =
                make_label_scan(std::move(plan_.root), "LabelScan(" + node.var + ":" + node.labels[0] + ")",
                                node.slot, node.labels[0]);
            scanned_labels = 1;
        } else {
            plan_.root = make_node_scan(std::move(plan_.root), "NodeScan(" + node.var + ")", node.slot);
        }
        mark_bound(node.slot);
        plan_label_filter(node, scanned_labels);
        flush_filters(state);
    }

    static Direction hop_direction(RelDirection direction, bool reversed) {
        if (direction == RelDirection::Both)
            return Direction::Both;
        return (direction == RelDirection::Right) != reversed ? Direction::Out : Direction::In;
    }

    static std::string arrow_of(Direction direction) {
        if (direction == Direction::Out)
            return "->";
        return direction == Direction::In ? "<-" : "--";
    }

    void plan_hop(MatchState& state, const RelPattern& rel, const NodePattern& from, const NodePattern& to,
                  bool reversed) {
        // Hedef bağlıysa Expand yalnız kimlik karşılaştırır: düğüm olmayan değer sessizce "eşleşmedi"
        // olmasın.
        if (is_bound(to.slot) && kind_of(to.slot) == VarKind::Value)
            plan_.root = make_check_node(std::move(plan_.root), to.slot, to.pos);
        ExpandSpec spec;
        spec.src = from.slot;
        spec.dst = to.slot;
        spec.dst_bound = is_bound(to.slot);
        spec.direction = hop_direction(rel.direction, reversed);
        spec.types = type_set_of(rel.types);
        spec.min_hops = rel.min_hops;
        spec.max_hops = rel.max_hops;
        spec.pos = rel.pos;
        spec.earlier_edges = state.used_edges;
        spec.earlier_paths = state.used_paths;
        const std::string ends = "(" + from.var + arrow_of(spec.direction) + to.var + ")";
        ++state.planned_rels;
        if (rel.var_length) {
            // Yol listesi yalnız adı varsa veya sonraki kenar benzersizlik için bakacaksa kurulur: satır
            // başına ayırma.
            const bool keep_path = !rel.var.empty() || state.planned_rels < state.total_rels;
            spec.edge = keep_path ? rel.slot : -1;
            spec.name = "VarLengthExpand" + ends;
            plan_.root = make_var_length_expand(std::move(plan_.root), std::move(spec));
            if (keep_path)
                state.used_paths.push_back(rel.slot);
        } else {
            spec.edge = rel.slot;
            spec.name = (spec.dst_bound ? "ExpandInto" : "Expand") + ends;
            plan_.root = make_expand(std::move(plan_.root), std::move(spec));
            state.used_edges.push_back(rel.slot);
        }
        mark_bound(rel.slot);
        mark_bound(to.slot);
        plan_label_filter(to, 0);
        flush_filters(state);
    }

    void plan_path(MatchState& state, const PathPattern& path, std::size_t start) {
        plan_start(state, path, path.nodes[start]);
        for (std::size_t i = start; i < path.rels.size(); ++i)
            plan_hop(state, path.rels[i], path.nodes[i], path.nodes[i + 1], false);
        for (std::size_t i = start; i > 0; --i)
            plan_hop(state, path.rels[i - 1], path.nodes[i], path.nodes[i - 1], true);
    }

    // Açgözlü: en kısıtlı başlangıcı olan yol önce; eşitlikte soldaki. Bağlanan değişken sonrakilerin puanını
    // artırır.
    void plan_remaining_paths(MatchState& state, const std::vector<PathPattern*>& paths) {
        std::vector<bool> planned(paths.size(), false);
        for (std::size_t round = 0; round < paths.size(); ++round) {
            int best_score = -1;
            std::size_t best_path = 0;
            std::size_t best_node = 0;
            for (std::size_t p = 0; p < paths.size(); ++p) {
                if (planned[p])
                    continue;
                for (std::size_t n = 0; n < paths[p]->nodes.size(); ++n) {
                    const int score = start_score(state, paths[p]->nodes[n]);
                    if (score > best_score) {
                        best_score = score;
                        best_path = p;
                        best_node = n;
                    }
                }
            }
            planned[best_path] = true;
            plan_path(state, *paths[best_path], best_node);
        }
    }

    void plan_pattern_paths(const std::vector<PathPattern*>& paths, ExprPtr where) {
        MatchState state;
        std::set<std::string, std::less<>> rel_vars;
        for (PathPattern* path : paths) {
            for (NodePattern& node : path->nodes)
                declare_node(node);
            for (RelPattern& rel : path->rels) {
                declare_rel(rel, rel_vars);
                ++state.total_rels;
            }
        }
        if (error_)
            return;
        for (PathPattern* path : paths) {
            for (const NodePattern& node : path->nodes)
                add_inline_filters(state, node.slot, node.props, node.pos);
            for (const RelPattern& rel : path->rels)
                add_inline_filters(state, rel.slot, rel.props, rel.pos);
        }
        if (where)
            split_where(state, std::move(where));
        if (error_)
            return;
        // Yalnız parametre veya sabite bağlı koşullar taramadan önce iner.
        flush_filters(state);
        plan_remaining_paths(state, paths);
        flush_filters(state);
    }

    void plan_pattern(std::vector<PathPattern>& paths, ExprPtr& where) {
        std::vector<PathPattern*> pointers;
        for (PathPattern& path : paths)
            pointers.push_back(&path);
        plan_pattern_paths(pointers, std::move(where));
    }

    // --- Yazma adımları ---

    std::vector<WriteProperty> write_properties(const PropertyMap& props) {
        std::vector<WriteProperty> out;
        for (const auto& [key, value] : props)
            out.push_back(WriteProperty{key, value.get()});
        return out;
    }

    void check_write_rel(const RelPattern& rel) {
        if (rel.var_length)
            syntax("variable-length relationship cannot be written", rel.pos);
        if (rel.types.size() != 1)
            syntax("written relationship needs exactly one type", rel.pos);
    }

    WriteEdge make_write_edge(const RelPattern& rel, const NodePattern& left, const NodePattern& right) {
        WriteEdge edge;
        edge.slot = rel.slot;
        edge.pos = rel.pos;
        edge.src = rel.direction == RelDirection::Left ? right.slot : left.slot;
        edge.dst = rel.direction == RelDirection::Left ? left.slot : right.slot;
        if (!rel.types.empty())
            edge.type = rel.types[0];
        edge.props = write_properties(rel.props);
        return edge;
    }

    void plan_create_node(PathPattern& path, NodePattern& node, std::vector<WriteStep>& steps) {
        const bool already_bound = !node.var.empty() && scope_.contains(node.var);
        declare_node(node);
        if (already_bound && (!node.labels.empty() || !node.props.empty() || path.rels.empty()))
            syntax("variable is already bound: " + node.var, node.pos);
        resolve_properties(node.props, scope_);
        mark_bound(node.slot);
        WriteNode step;
        step.slot = node.slot;
        step.bound = already_bound;
        step.pos = node.pos;
        step.labels = node.labels;
        step.props = write_properties(node.props);
        steps.emplace_back(std::move(step));
    }

    void plan_create_path(PathPattern& path, std::vector<WriteStep>& steps) {
        std::set<std::string, std::less<>> rel_vars;
        plan_create_node(path, path.nodes[0], steps);
        for (std::size_t i = 0; i < path.rels.size(); ++i) {
            RelPattern& rel = path.rels[i];
            check_write_rel(rel);
            if (rel.direction == RelDirection::Both)
                syntax("created relationship needs a direction", rel.pos);
            plan_create_node(path, path.nodes[i + 1], steps);
            declare_rel(rel, rel_vars);
            resolve_properties(rel.props, scope_);
            mark_bound(rel.slot);
            steps.emplace_back(make_write_edge(rel, path.nodes[i], path.nodes[i + 1]));
        }
    }

    void check_merge_pattern(const PathPattern& path) {
        for (const RelPattern& rel : path.rels)
            check_write_rel(rel);
        for (const NodePattern& node : path.nodes)
            if (!node.var.empty() && scope_.contains(node.var) &&
                (!node.labels.empty() || !node.props.empty()))
                syntax("variable is already bound: " + node.var, node.pos);
    }

    std::vector<WriteStep> merge_steps(const PathPattern& path,
                                       const std::set<std::string, std::less<>>& visible) {
        std::vector<WriteStep> steps;
        std::set<int> created;
        const auto add_node = [&](const NodePattern& node) {
            WriteNode step;
            step.slot = node.slot;
            step.pos = node.pos;
            step.bound =
                (!node.var.empty() && visible.contains(node.var)) || !created.insert(node.slot).second;
            step.labels = node.labels;
            step.props = write_properties(node.props);
            steps.emplace_back(std::move(step));
        };
        add_node(path.nodes[0]);
        for (std::size_t i = 0; i < path.rels.size(); ++i) {
            add_node(path.nodes[i + 1]);
            steps.emplace_back(make_write_edge(path.rels[i], path.nodes[i], path.nodes[i + 1]));
        }
        return steps;
    }

    Plan& plan_;
    const Params& params_;
    const QueryLimits& limits_;
    std::size_t plan_nodes_ = 0;
    std::size_t param_bytes_ = 0;
    Scope scope_;
    std::vector<bool> bound_;
    std::vector<VarKind> kinds_;
    std::map<std::string, int, std::less<>> key_ids_;
    std::optional<Error> error_;
};

}  // namespace

Result<Plan> build_plan(std::string_view cypher, const Params& params, const QueryLimits& limits) {
    Result<Query> query = parse_query(cypher.data(), cypher.size());
    if (!query)
        return std::unexpected(std::move(query).error());
    Plan plan;
    plan.query = std::move(*query);
    if (const Result<void> planned = Planner(plan, params, limits).run(); !planned)
        return std::unexpected(planned.error());
    return plan;
}

Result<std::vector<std::string>> describe_plan(std::string_view cypher, const Params& params) {
    Result<Plan> plan = build_plan(cypher, params, QueryLimits{});
    if (!plan)
        return std::unexpected(std::move(plan).error());
    std::vector<std::string> names;
    plan->root->describe(names);
    return names;
}

}  // namespace noteapp::graph::detail::cypher
