// Yazma işleçleri. Tek yazma yolu WriteTxn'dir: her değişiklik aynı işlemde günlüğe girer.
#include "graph/cypher/expr.hpp"
#include "graph/cypher/operators.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::graph::detail::cypher {

namespace {

constexpr std::string_view kMergeNodeNamespace = "cypher.merge.node";
constexpr std::string_view kMergeEdgeNamespace = "cypher.merge.edge";

Error bad_target(Position pos) {
    return located_error(Error::Code::InvalidArgument, "graph.cypher_bad_target", pos);
}

Error merge_conflict(Position pos) {
    return located_error(Error::Code::InvalidArgument, "graph.cypher_merge_conflict", pos);
}

Error merge_null(Position pos, const std::string& key) {
    return located_error(Error::Code::InvalidArgument, "graph.cypher_merge_null", pos, {{"key", key}});
}

// Uygulamanın denetimi (QueryHooks): her değişiklikten önce. Ret, değişikliğin sorgudaki konumunu taşır.
Result<void> check_change(ExecContext& context, const QueryChange& change, Position pos) {
    if (context.hooks == nullptr || !context.hooks->before_change)
        return {};
    Result<void> allowed = context.hooks->before_change(context.read, change);
    if (allowed)
        return {};
    Error error = std::move(allowed).error();
    error.args.emplace_back("line", std::to_string(pos.line));
    error.args.emplace_back("col", std::to_string(pos.col));
    return std::unexpected(std::move(error));
}

Result<void> put_node(ExecContext& context, const NodeId& id, Position pos) {
    CYPHER_TRY(check_change(context, {QueryChange::Kind::CreateNode, id}, pos));
    return context.write->put_node(id);
}

Result<void> put_edge(ExecContext& context, const EdgeId& id, const NodeId& src, const std::string& type,
                      const NodeId& dst, Position pos) {
    CYPHER_TRY(check_change(context, {QueryChange::Kind::CreateEdge, id, type}, pos));
    return context.write->put_edge(id, src, type, dst);
}

Result<void> set_property(ExecContext& context, const OwnerId& owner, const std::string& key, Value value,
                          Position pos) {
    CYPHER_TRY(check_change(context, {QueryChange::Kind::SetProperty, owner, key, &value}, pos));
    return context.write->set_property(owner, key, std::move(value));
}

Result<void> change_label(ExecContext& context, const NodeId& id, const std::string& label, bool remove,
                          Position pos) {
    using Kind = QueryChange::Kind;
    CYPHER_TRY(check_change(context, {remove ? Kind::RemoveLabel : Kind::AddLabel, id, label}, pos));
    return remove ? context.write->remove_label(id, label) : context.write->add_label(id, label);
}

// Sıra anahtarlıdır: desen hangi sırayla yazılırsa yazılsın kimlik aynı kalır.
struct EvaluatedProperty {
    std::string key;
    QueryValue value;
};

Result<std::vector<EvaluatedProperty>> eval_merge_properties(const std::vector<WriteProperty>& props,
                                                             const Row& row, Position pos,
                                                             ExecContext& context) {
    std::vector<EvaluatedProperty> out;
    for (const WriteProperty& prop : props) {
        Result<QueryValue> value = eval(*prop.value, row, context);
        if (!value)
            return std::unexpected(std::move(value).error());
        if (value->is_null())
            return std::unexpected(merge_null(pos, prop.key));
        const Result<Value> storable = to_stored(*value, pos);
        if (!storable)
            return std::unexpected(storable.error());
        out.push_back(EvaluatedProperty{prop.key, std::move(*value)});
    }
    std::ranges::sort(out, {}, &EvaluatedProperty::key);
    return out;
}

void append_properties(std::string& canonical, const std::vector<EvaluatedProperty>& props) {
    for (const EvaluatedProperty& prop : props) {
        append_canonical(canonical, prop.key);
        append_canonical(canonical, prop.value);
    }
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

Result<void> set_properties(ExecContext& context, const OwnerId& owner,
                            const std::vector<EvaluatedProperty>& props, Position pos) {
    for (const EvaluatedProperty& prop : props) {
        Result<Value> stored = to_stored(prop.value, pos);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        CYPHER_TRY(set_property(context, owner, prop.key, std::move(*stored), pos));
    }
    return {};
}

// Türetilmiş kimlik doluysa yalnız desenle uyuşan kullanılır: uyuşmayanın üstüne yazmak veriyi sessizce geri
// alırdı.
Result<void> require_same_properties(ReadTxn& txn, const OwnerId& owner,
                                     const std::vector<EvaluatedProperty>& props, Position pos) {
    for (const EvaluatedProperty& prop : props) {
        Result<Value> stored = txn.property(owner, prop.key);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        if (values_equal(from_stored(std::move(*stored)), prop.value) != std::optional<bool>(true))
            return std::unexpected(merge_conflict(pos));
    }
    return {};
}

Result<void> require_labels(ReadTxn& txn, const NodeId& id, const std::vector<std::string>& labels,
                            Position pos) {
    const Result<std::vector<std::string>> have = txn.labels(id);
    if (!have)
        return std::unexpected(have.error());
    for (const std::string& label : labels)
        if (std::ranges::find(*have, label) == have->end())
            return std::unexpected(merge_conflict(pos));
    return {};
}

Result<void> add_labels(ExecContext& context, const NodeId& id, const std::vector<std::string>& labels,
                        Position pos) {
    for (const std::string& label : labels)
        CYPHER_TRY(change_label(context, id, label, false, pos));
    return {};
}

// Etiket ve özellik kanonik sırada: iki cihaz aynı deseni kurarsa aynı kimliğe varır.
Result<void> merge_node(const WriteNode& step, Row& row, ExecContext& context) {
    Result<std::vector<EvaluatedProperty>> props = eval_merge_properties(step.props, row, step.pos, context);
    if (!props)
        return std::unexpected(std::move(props).error());
    std::vector<std::string> labels = step.labels;
    std::ranges::sort(labels);
    const auto duplicates = std::ranges::unique(labels);
    labels.erase(duplicates.begin(), duplicates.end());

    std::string canonical;
    for (const std::string& label : labels)
        append_canonical(canonical, label);
    canonical += '\0';
    append_properties(canonical, *props);
    const NodeId id = derive_node_id(kMergeNodeNamespace, bytes_of(canonical));

    const Result<bool> exists = context.read.has_node(id);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists) {
        CYPHER_TRY(require_labels(context.read, id, labels, step.pos));
        CYPHER_TRY(require_same_properties(context.read, id, *props, step.pos));
    } else {
        CYPHER_TRY(put_node(context, id, step.pos));
        CYPHER_TRY(add_labels(context, id, labels, step.pos));
        CYPHER_TRY(set_properties(context, id, *props, step.pos));
    }
    row[static_cast<std::size_t>(step.slot)] = id;
    return {};
}

Result<void> create_node(const WriteNode& step, Row& row, ExecContext& context) {
    const NodeId id = new_node_id();
    CYPHER_TRY(put_node(context, id, step.pos));
    CYPHER_TRY(add_labels(context, id, step.labels, step.pos));
    row[static_cast<std::size_t>(step.slot)] = id;
    for (const WriteProperty& prop : step.props) {
        const Result<QueryValue> value = eval(*prop.value, row, context);
        if (!value)
            return std::unexpected(value.error());
        // CREATE'te null yazılmaz: özelliğin yokluğudur.
        if (value->is_null())
            continue;
        Result<Value> stored = to_stored(*value, step.pos);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        CYPHER_TRY(set_property(context, id, prop.key, std::move(*stored), step.pos));
    }
    return {};
}

Result<NodeId> endpoint(const Row& row, int slot, Position pos) {
    const auto* node = std::get_if<NodeId>(&row[static_cast<std::size_t>(slot)].v);
    if (node == nullptr)
        return std::unexpected(bad_target(pos));
    return *node;
}

Result<void> merge_edge(const WriteEdge& step, Row& row, const NodeId& src, const NodeId& dst,
                        ExecContext& context) {
    Result<std::vector<EvaluatedProperty>> props = eval_merge_properties(step.props, row, step.pos, context);
    if (!props)
        return std::unexpected(std::move(props).error());
    std::string canonical;
    append_canonical(canonical, src);
    append_canonical(canonical, step.type);
    append_canonical(canonical, dst);
    append_properties(canonical, *props);
    const EdgeId id = derive_edge_id(kMergeEdgeNamespace, bytes_of(canonical));

    const Result<std::optional<std::string>> existing = read_edge_type(context, id);
    if (!existing)
        return std::unexpected(existing.error());
    if (*existing) {
        CYPHER_TRY(require_same_properties(context.read, id, *props, step.pos));
    } else {
        CYPHER_TRY(put_edge(context, id, src, step.type, dst, step.pos));
        CYPHER_TRY(set_properties(context, id, *props, step.pos));
    }
    row[static_cast<std::size_t>(step.slot)] = id;
    return {};
}

Result<void> create_edge(const WriteEdge& step, Row& row, const NodeId& src, const NodeId& dst,
                         ExecContext& context) {
    const EdgeId id = new_edge_id();
    CYPHER_TRY(put_edge(context, id, src, step.type, dst, step.pos));
    row[static_cast<std::size_t>(step.slot)] = id;
    for (const WriteProperty& prop : step.props) {
        const Result<QueryValue> value = eval(*prop.value, row, context);
        if (!value)
            return std::unexpected(value.error());
        if (value->is_null())
            continue;
        Result<Value> stored = to_stored(*value, step.pos);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        CYPHER_TRY(set_property(context, id, prop.key, std::move(*stored), step.pos));
    }
    return {};
}

Result<void> run_node_step(const WriteNode& step, Row& row, bool merging, ExecContext& context) {
    if (step.bound) {
        const QueryValue& value = row[static_cast<std::size_t>(step.slot)];
        if (!std::holds_alternative<NodeId>(value.v))
            return std::unexpected(bad_target(step.pos));
        return {};
    }
    return merging ? merge_node(step, row, context) : create_node(step, row, context);
}

Result<void> run_edge_step(const WriteEdge& step, Row& row, bool merging, ExecContext& context) {
    const Result<NodeId> src = endpoint(row, step.src, step.pos);
    if (!src)
        return std::unexpected(src.error());
    const Result<NodeId> dst = endpoint(row, step.dst, step.pos);
    if (!dst)
        return std::unexpected(dst.error());
    return merging ? merge_edge(step, row, *src, *dst, context) : create_edge(step, row, *src, *dst, context);
}

Result<void> run_steps(const std::vector<WriteStep>& steps, Row& row, bool merging, ExecContext& context) {
    for (const WriteStep& step : steps) {
        CYPHER_TRY(context.budget.tick());
        if (const auto* node = std::get_if<WriteNode>(&step.what))
            CYPHER_TRY(run_node_step(*node, row, merging, context));
        else
            CYPHER_TRY(run_edge_step(std::get<WriteEdge>(step.what), row, merging, context));
    }
    return {};
}

// Girdi önce tümüyle okunur: açık tarama yazmayla çakışmaz. Sonra hepsi yazılır, downstream az satır istese
// de.
class EagerWriteOp : public PipelineOp {
public:
    EagerWriteOp(std::string name, OperatorPtr child) : PipelineOp(std::move(name), std::move(child)) {}

    Result<bool> next(Row& row, ExecContext& context) final {
        if (!done_) {
            CYPHER_TRY(materialize(row, context));
            CYPHER_TRY(write_all(context));
            done_ = true;
        }
        if (cursor_ >= rows_.size())
            return false;
        row = std::move(rows_[cursor_++]);
        return true;
    }

protected:
    virtual Result<void> write_all(ExecContext& context) = 0;
    std::vector<Row> rows_;

private:
    Result<void> materialize(Row& row, ExecContext& context) {
        while (true) {
            const Result<bool> more = child_->next(row, context);
            if (!more)
                return std::unexpected(more.error());
            if (!*more)
                return {};
            CYPHER_TRY(context.budget.charge(row_bytes(row)));
            rows_.push_back(row);
        }
    }

    std::size_t cursor_ = 0;
    bool done_ = false;
};

class CreateOp final : public EagerWriteOp {
public:
    CreateOp(OperatorPtr child, std::vector<WriteStep> steps)
        : EagerWriteOp("Create", std::move(child)), steps_(std::move(steps)) {}

private:
    Result<void> write_all(ExecContext& context) override {
        for (Row& row : rows_)
            CYPHER_TRY(run_steps(steps_, row, false, context));
        return {};
    }

    std::vector<WriteStep> steps_;
};

class MergeOp final : public EagerWriteOp {
public:
    MergeOp(OperatorPtr child, OperatorPtr inner, OnceOp* leaf, std::vector<WriteStep> steps)
        : EagerWriteOp("Merge", std::move(child)), inner_(std::move(inner)), leaf_(leaf),
          steps_(std::move(steps)) {}

    void describe(std::vector<std::string>& out) const override {
        child_->describe(out);
        std::vector<std::string> inner;
        inner_->describe(inner);
        std::string name = name_ + "[";
        for (std::size_t i = 0; i < inner.size(); ++i)
            name += (i == 0 ? "" : ",") + inner[i];
        out.push_back(name + "]");
    }

private:
    Result<void> write_all(ExecContext& context) override {
        std::vector<Row> output;
        for (Row& row : rows_)
            CYPHER_TRY(merge_row(row, output, context));
        rows_ = std::move(output);
        return {};
    }

    // Önceki satırın kurduğu eşleşmeler görünür: iç plan her satırda yeniden çalışır.
    Result<void> merge_row(Row& row, std::vector<Row>& output, ExecContext& context) {
        leaf_->rearm();
        Row current = row;
        bool matched = false;
        while (true) {
            const Result<bool> more = inner_->next(current, context);
            if (!more)
                return std::unexpected(more.error());
            if (!*more)
                break;
            matched = true;
            CYPHER_TRY(context.budget.charge(row_bytes(current)));
            output.push_back(current);
        }
        if (matched)
            return {};
        CYPHER_TRY(run_steps(steps_, row, true, context));
        output.push_back(std::move(row));
        return {};
    }

    OperatorPtr inner_;
    OnceOp* leaf_;
    std::vector<WriteStep> steps_;
};

class SetOp final : public EagerWriteOp {
public:
    SetOp(OperatorPtr child, std::string name, bool remove, std::vector<SetOperation> operations)
        : EagerWriteOp(std::move(name), std::move(child)), remove_(remove),
          operations_(std::move(operations)) {}

private:
    Result<void> write_all(ExecContext& context) override {
        for (const Row& row : rows_)
            for (const SetOperation& operation : operations_)
                CYPHER_TRY(apply(operation, row, context));
        return {};
    }

    Result<void> apply(const SetOperation& operation, const Row& row, ExecContext& context) const {
        CYPHER_TRY(context.budget.tick());
        const Result<QueryValue> target = eval(*operation.target, row, context);
        if (!target)
            return std::unexpected(target.error());
        // OPTIONAL MATCH'ten gelen null hedef sessizce geçilir (openCypher).
        if (target->is_null())
            return {};
        if (!operation.key.empty())
            return apply_property(operation, *target, row, context);
        const auto* node = std::get_if<NodeId>(&target->v);
        if (node == nullptr)
            return std::unexpected(bad_target(operation.pos));
        for (const std::string& label : operation.labels)
            CYPHER_TRY(change_label(context, *node, label, remove_, operation.pos));
        return {};
    }

    static Result<void> apply_property(const SetOperation& operation, const QueryValue& target,
                                       const Row& row, ExecContext& context) {
        std::optional<OwnerId> owner;
        if (const auto* node = std::get_if<NodeId>(&target.v))
            owner = OwnerId(*node);
        else if (const auto* edge = std::get_if<EdgeId>(&target.v))
            owner = OwnerId(*edge);
        if (!owner)
            return std::unexpected(bad_target(operation.pos));
        if (operation.value == nullptr)  // null yazmak kaldırır
            return set_property(context, *owner, operation.key, Value{}, operation.pos);
        const Result<QueryValue> value = eval(*operation.value, row, context);
        if (!value)
            return std::unexpected(value.error());
        Result<Value> stored = to_stored(*value, operation.pos);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        return set_property(context, *owner, operation.key, std::move(*stored), operation.pos);
    }

    bool remove_;
    std::vector<SetOperation> operations_;
};

class DeleteOp final : public EagerWriteOp {
public:
    DeleteOp(OperatorPtr child, bool detach, std::vector<const Expr*> targets)
        : EagerWriteOp("Delete", std::move(child)), detach_(detach), targets_(std::move(targets)) {}

private:
    // Silinecek öğe ve onu veren ifadenin konumu (denetimin reddi orayı gösterir).
    template <class Id> struct Target {
        Id id;
        Position pos;
    };
    template <class Id> static void sort_unique(std::vector<Target<Id>>& targets) {
        std::ranges::sort(targets, {}, &Target<Id>::id);
        const auto duplicates = std::ranges::unique(targets, {}, &Target<Id>::id);
        targets.erase(duplicates.begin(), duplicates.end());
    }

    Result<void> write_all(ExecContext& context) override {
        std::vector<Target<NodeId>> nodes;
        std::vector<Target<EdgeId>> edges;
        for (const Row& row : rows_)
            for (const Expr* target : targets_)
                CYPHER_TRY(collect(*target, row, nodes, edges, context));
        // Cümle sonunda birlikte silinir: DELETE a, r yazım sırasına bağlı olmasın, aynı hedef iki kez
        // gelebilir.
        sort_unique(edges);
        sort_unique(nodes);
        CYPHER_TRY(delete_edges(edges, context));
        return delete_nodes(nodes, context);
    }

    static Result<void> collect_value(const QueryValue& value, Position pos, std::vector<Target<NodeId>>& nodes,
                                      std::vector<Target<EdgeId>>& edges) {
        if (value.is_null())
            return {};
        if (const auto* node = std::get_if<NodeId>(&value.v)) {
            nodes.push_back({*node, pos});
        } else if (const auto* edge = std::get_if<EdgeId>(&value.v)) {
            edges.push_back({*edge, pos});
        } else if (const auto* list = std::get_if<QueryList>(&value.v)) {
            for (const QueryValue& item : list->items)
                CYPHER_TRY(collect_value(item, pos, nodes, edges));
        } else {
            return std::unexpected(bad_target(pos));
        }
        return {};
    }

    static Result<void> collect(const Expr& target, const Row& row, std::vector<Target<NodeId>>& nodes,
                                std::vector<Target<EdgeId>>& edges, ExecContext& context) {
        const Result<QueryValue> value = eval(target, row, context);
        if (!value)
            return std::unexpected(value.error());
        return collect_value(*value, target.pos, nodes, edges);
    }

    static Result<void> delete_edges(const std::vector<Target<EdgeId>>& edges, ExecContext& context) {
        for (const auto& [edge, pos] : edges) {
            CYPHER_TRY(context.budget.tick());
            const Result<std::optional<std::string>> type = read_edge_type(context, edge);
            if (!type)
                return std::unexpected(type.error());
            if (!*type)
                continue;
            CYPHER_TRY(check_change(context, {QueryChange::Kind::DeleteEdge, edge, **type}, pos));
            CYPHER_TRY(context.write->delete_edge(edge));
        }
        return {};
    }

    // DETACH'ın sileceği kenarlar da denetlenir: uygulama silinen kenarın tipini bilmeli. Yalnız denetim
    // varken okunur.
    Result<void> check_detached_edges(const NodeId& node, Position pos, ExecContext& context) const {
        if (!detach_ || context.hooks == nullptr || !context.hooks->before_change)
            return {};
        std::vector<std::pair<EdgeId, std::string>> attached;
        {
            Result<NeighborCursor> cursor = context.read.neighbors(node, Direction::Both);
            if (!cursor)
                return std::unexpected(std::move(cursor).error());
            while (true) {
                const Result<std::optional<Neighbor>> next = cursor->next();
                if (!next)
                    return std::unexpected(next.error());
                if (!*next)
                    break;
                CYPHER_TRY(context.budget.expansion());
                attached.emplace_back((*next)->edge, std::string((*next)->type));
            }
        }
        for (const auto& [edge, type] : attached)
            CYPHER_TRY(check_change(context, {QueryChange::Kind::DeleteEdge, edge, type}, pos));
        return {};
    }

    Result<void> delete_nodes(const std::vector<Target<NodeId>>& nodes, ExecContext& context) const {
        for (const auto& [node, pos] : nodes) {
            // DETACH düğüm başına kenarları da siler: saat burada daha sık bakar.
            CYPHER_TRY(context.budget.expansion());
            const Result<bool> exists = context.read.has_node(node);
            if (!exists)
                return std::unexpected(exists.error());
            if (!*exists)
                continue;
            CYPHER_TRY(check_change(context, {QueryChange::Kind::DeleteNode, node}, pos));
            CYPHER_TRY(check_detached_edges(node, pos, context));
            CYPHER_TRY(context.write->delete_node(node, detach_));
        }
        return {};
    }

    bool detach_;
    std::vector<const Expr*> targets_;
};

}  // namespace

OperatorPtr make_create(OperatorPtr child, std::vector<WriteStep> steps) {
    return std::make_unique<CreateOp>(std::move(child), std::move(steps));
}

OperatorPtr make_merge(OperatorPtr child, OperatorPtr inner, OnceOp* inner_leaf,
                       std::vector<WriteStep> steps) {
    return std::make_unique<MergeOp>(std::move(child), std::move(inner), inner_leaf, std::move(steps));
}

OperatorPtr make_set(OperatorPtr child, std::string name, bool remove, std::vector<SetOperation> operations) {
    return std::make_unique<SetOp>(std::move(child), std::move(name), remove, std::move(operations));
}

OperatorPtr make_delete(OperatorPtr child, bool detach, std::vector<const Expr*> targets) {
    return std::make_unique<DeleteOp>(std::move(child), detach, std::move(targets));
}

}  // namespace noteapp::graph::detail::cypher
