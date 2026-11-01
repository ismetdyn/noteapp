#include "sync/note_status.hpp"

#include "core/note_sync.hpp"
#include "sync/sync_service.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace noteapp::sync {

namespace {

constexpr std::string_view kConflictMarker = " (conflict ";
constexpr std::string_view kStampPattern = "dddd-dd-dd dddd)";  // d = rakam

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool matches_stamp(std::string_view text) {
    return text.size() >= kStampPattern.size() &&
           std::ranges::equal(text.substr(0, kStampPattern.size()), kStampPattern, [](char c, char pattern) {
               return pattern == 'd' ? is_digit(c) : c == pattern;
           });
}

// Damgadan sonrası: isteğe bağlı " N", sonra isteğe bağlı tek uzantı.
bool valid_copy_tail(std::string_view tail) {
    if (tail.starts_with(' ') || tail.starts_with('-')) {  // " N": eski dosya kopyası, "-N": yol çakışması
        tail.remove_prefix(1);
        const auto digits = std::ranges::distance(tail.begin(), std::ranges::find_if_not(tail, is_digit));
        if (digits == 0)
            return false;
        tail.remove_prefix(static_cast<std::size_t>(digits));
    }
    return tail.empty() || (tail.front() == '.' && tail.find('.', 1) == std::string_view::npos);
}

}  // namespace

bool is_conflict_copy_name(std::string_view path) {
    const auto slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto marker = name.rfind(kConflictMarker);
    if (marker == std::string_view::npos)
        return false;
    const std::string_view rest = name.substr(marker + kConflictMarker.size());
    return matches_stamp(rest) && valid_copy_tail(rest.substr(kStampPattern.size()));
}

std::expected<void, core::Error> fill_sync_status(const core::Config& config,
                                                  std::span<core::NoteMeta> notes) {
    std::ranges::for_each(notes, [](core::NoteMeta& note) { note.sync_status = core::SyncState::SyncOff; });
    if (!config.sync.enabled)
        return {};

    std::error_code ec;
    if (!std::filesystem::exists(config.notes_dir / ".git", ec))
        return {};
    // Ölçüt: durum dosyasındaki son gönderilen işlem. Bundan büyük kendi yazması olan not henüz uzakta
    // değildir.
    if (notes.empty())
        return {};
    core::NoteStore store = core::NoteStore::for_config(config);
    const auto own_log = core::own_log_name(store);
    if (!own_log)
        return std::unexpected(own_log.error());
    const std::uint64_t pushed = SyncService(config).pushed_seq_of(*own_log);
    for (core::NoteMeta& note : notes) {
        if (is_conflict_copy_name(note.path.string()))
            note.sync_status = core::SyncState::ConflictCopy;
        else if (note.created_local_seq > pushed)
            note.sync_status = core::SyncState::New;
        else if (note.local_seq > pushed)
            note.sync_status = core::SyncState::Modified;
        else
            note.sync_status = core::SyncState::Synced;
    }
    return {};
}

}  // namespace noteapp::sync
