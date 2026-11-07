#include "app/commands.hpp"

#include "core/i18n.hpp"
#include "search/fuzzy.hpp"

#include <limits>
#include <tuple>

namespace noteapp::app {

CommandBlock command_block(CommandNeeds needs, const CommandState& state) {
    const auto unless = [](bool satisfied, CommandBlock reason) { return satisfied ? CommandBlock::None : reason; };
    switch (needs) {
    case CommandNeeds::Nothing:
        return CommandBlock::None;
    case CommandNeeds::Note:
        return unless(state.note, CommandBlock::NoNote);
    case CommandNeeds::Row:
        return unless(state.row, CommandBlock::NoRow);
    case CommandNeeds::Links:
        return unless(state.links, CommandBlock::NoLinks);
    case CommandNeeds::FolderView:
        return unless(state.folder_view, CommandBlock::NotFolderView);
    case CommandNeeds::Yank:
        return state.yank ? unless(state.paste_folder, CommandBlock::PasteNeedsFolder) : CommandBlock::NothingYanked;
    case CommandNeeds::Sync:
        return unless(state.sync, CommandBlock::SyncOff);
    }
    return CommandBlock::None;
}

std::vector<std::size_t> rank_palette(std::string_view query, std::span<const PaletteCandidate> candidates,
                                      std::span<const std::string> recent, std::size_t max_candidate) {
    struct Ranked {
        std::size_t index;
        bool blocked;
        double score;
        std::size_t recency;
    };
    std::vector<Ranked> ranked;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const PaletteCandidate& candidate = candidates[i];
        const auto by_id = search::fuzzy_match(query, candidate.id, max_candidate);
        const auto by_label = search::fuzzy_match(query, core::tr(candidate.label_key), max_candidate);
        if (!by_id && !by_label)
            continue;
        const auto used = std::ranges::find(recent, candidate.id);
        ranked.push_back({i, candidate.blocked,
                          std::max(by_id ? by_id->score : std::numeric_limits<double>::lowest(),
                                   by_label ? by_label->score : std::numeric_limits<double>::lowest()),
                          static_cast<std::size_t>(used - recent.begin())});
    }
    // Boş süzgeçte puan 0'dır: sırayı son kullanılan belirler. Çalışmayan satır aşağıda ama görünür kalır.
    std::ranges::stable_sort(ranked, [](const Ranked& a, const Ranked& b) {
        return std::tuple(a.blocked, -a.score, a.recency) < std::tuple(b.blocked, -b.score, b.recency);
    });
    std::vector<std::size_t> order;
    order.reserve(ranked.size());
    for (const Ranked& item : ranked)
        order.push_back(item.index);
    return order;
}

void remember_command(std::vector<std::string>& recent, std::string_view id, const core::Limit& limit) {
    std::erase(recent, id);
    recent.insert(recent.begin(), std::string(id));
    if (recent.size() > limit.cap())
        recent.resize(limit.cap());
}

}  // namespace noteapp::app
