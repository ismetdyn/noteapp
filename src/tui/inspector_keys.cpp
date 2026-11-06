// Özellik denetçisinin tuşları (F4; SPEC-tui "Denetçi"). Harfler İngilizce sözcüklerden: p(roperties) açar,
// a(dd), e(dit), d(elete); depo işi ListActions'ta (Intent::Inspector*).
#include "tui/tui_state.hpp"

#include <algorithm>

namespace noteapp::tui {

namespace {

constexpr int kInspectorMinRows = 3;
constexpr int kInspectorMaxRows = 12;  // bağlantı bölümünün sınırları (LinkPanel)

std::optional<core::NotePath> path_of(const app::MapNode& node) {
    if (node.kind != app::MapNodeKind::Note)
        return std::nullopt;
    auto path = core::NotePath::parse(node.key);
    return path ? std::optional(std::move(*path)) : std::nullopt;
}

}  // namespace

// Bölüm bağlantıların yerinde, panelin üçte biri (en az 3, en çok 12 satır) ve ayırıcı.
int TuiState::inspector_reserved_rows(int pane_rows) const {
    if (focus_ != Focus::Inspector || trash_view_)
        return 0;
    const int rows =
        static_cast<int>(std::min<std::size_t>(std::max<std::size_t>(inspector_.rows().size(), 1), 64));
    return std::min(rows, std::clamp(pane_rows / 3, kInspectorMinRows, kInspectorMaxRows)) + 1;
}

std::size_t TuiState::inspector_top(std::size_t visible) const {
    const std::size_t selected = inspector_.selected();
    return visible == 0 || selected < visible ? 0 : selected - visible + 1;
}

Intent TuiState::open_inspector(app::InspectTarget target) {
    inspector_back_ = focus_ == Focus::List ? Focus::List : Focus::Preview;
    inspector_.inspect(std::move(target));
    focus_ = Focus::Inspector;
    return Intent::InspectorLoad;
}

// REVIEW'da gösterilen not; bağlantı bölümünün ilişkili satırında o ilişki (notun gözünden yönüyle).
Intent TuiState::inspect_review() {
    const Row* row = selected_row();
    if (!row)
        return Intent::None;
    const core::LinkLine* line = links_.selected_line();
    if (focus_ == Focus::Links && line && !line->is_header && line->group == core::LinkGroup::Related) {
        const core::RelatedNote& relation = links_.links().related[line->index];
        const bool incoming = relation.direction == core::RelationDirection::Incoming;
        return open_inspector(app::InspectTarget::of_relation(
            relation.id, incoming ? relation.path : row->path, incoming ? row->path : relation.path,
            relation.name, relation.direction != core::RelationDirection::Undirected));
    }
    return open_inspector(app::InspectTarget::of_note(row->path));
}

// `C` kipinde hedefle kaynak arasındaki (ya da sağ tıklanan) kenar ilişkiyse o; yoksa Tab'la seçilen ya da
// gösterilen not.
Intent TuiState::inspect_map() {
    if (map_link_) {
        if (const auto found = map_link_edge(); found && found->edge.kind == app::MapEdgeKind::Related)
            return open_inspector(app::InspectTarget::of_relation(found->edge.relation, path_of(found->from),
                                                                  path_of(found->to), found->edge.name,
                                                                  found->edge.directed));
    }
    const auto target = map_target();
    if (!target)
        return Intent::None;
    auto path = core::NotePath::parse(*target);
    if (!path)
        return Intent::None;
    return open_inspector(app::InspectTarget::of_note(std::move(*path)));
}

Intent TuiState::handle_inspector(Key key, std::string_view text) {
    const int page = std::max(inspector_reserved_rows(pane_rows()) - 1, 1);
    switch (key) {
    case Key::Up:
        inspector_.move(-1);
        return Intent::None;
    case Key::Down:
        inspector_.move(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        inspector_.move((key == Key::PageUp ? -1 : 1) * page);
        return Intent::None;
    case Key::Enter:
        return Intent::InspectorEdit;
    case Key::Escape:
        focus_ = inspector_back_;
        return Intent::None;
    case Key::Char:
        break;
    default:
        return Intent::None;
    }
    if (handle_layout_letter(text))
        return Intent::None;
    if (text == "p") {
        focus_ = inspector_back_;
    } else if (text == "j" || text == "k") {
        inspector_.move(text == "j" ? 1 : -1);
    } else if (text == "a") {
        return Intent::InspectorAdd;
    } else if (text == "e") {
        return Intent::InspectorEdit;
    } else if (text == "d") {
        return Intent::InspectorDelete;
    }
    return Intent::None;
}

}  // namespace noteapp::tui
