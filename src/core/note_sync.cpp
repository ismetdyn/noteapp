#include "core/note_sync.hpp"

#include "core/i18n.hpp"
#include "core/note_store_impl.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <mutex>

namespace noteapp::core {

namespace {

std::string hex_of(const std::array<std::byte, 16>& bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::byte value : bytes)
        text += std::format("{:02x}", std::to_integer<unsigned>(value));
    return text;
}

// Değişen not sayısı: kullanıcının "kaç not geldi" sorusu. Kimlik, yol değişiminden etkilenmesin diye
// anahtardır; çöpe atma ve geri alma da değişikliktir, kalıcı silme sayılmaz.
int count_changes(const Snapshot* before, const Snapshot& after) {
    std::map<graph::NodeId, std::pair<Revision, bool>> known;
    if (before != nullptr) {
        for (const NoteRow& row : before->live)
            known.emplace(row.id, std::pair{row.info.revision, false});
        for (const NoteRow& row : before->trash)
            known.emplace(row.id, std::pair{row.info.revision, true});
    }
    int changed = 0;
    const auto count = [&](const std::vector<NoteRow>& rows, bool trashed) {
        for (const NoteRow& row : rows) {
            const auto found = known.find(row.id);
            changed += found == known.end() || found->second != std::pair{row.info.revision, trashed} ? 1 : 0;
        }
    };
    count(after.live, false);
    count(after.trash, true);
    return changed;
}

}  // namespace

std::expected<LogIngest, Error>
ingest_note_logs(NoteStore& store, std::span<const std::filesystem::path> logs, const TextMerger& merger) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    if (auto gate = impl.check_gate(); !gate)
        return std::unexpected(gate.error());
    if (auto opened = impl.open_for_write(); !opened)
        return std::unexpected(opened.error());
    const std::shared_ptr<const Snapshot> before = impl.current();
    if (!before && impl.last_error)
        return std::unexpected(*impl.last_error);
    auto merged = impl.opened_graph().ingest_logs(logs);
    if (!merged)
        return std::unexpected(from_graph(merged.error(), path_utf8(impl.database())));
    impl.snapshot.reset();

    LogIngest result{std::move(*merged), {}, {}, 0};
    const auto merged_view = impl.current();
    if (!merged_view)
        return std::unexpected(impl.last_error.value_or(io_error(path_utf8(impl.database()), "unreadable")));
    result.changed_notes = count_changes(before.get(), *merged_view);
    // Çakışan her not ayrı, her kopya ayrı işlem: biri çözülemezse (çerçeveye sığmıyor, disk) head'ler
    // yerinde kalır, raporlanır, diğerleri ve senkron sürer. Çerçeve içeriği ingest'i durduramaz (SPEC-graph
    // "Güvenilmez girdi").
    for (const std::vector<NoteRow>* rows : {&merged_view->live, &merged_view->trash})
        for (const NoteRow& row : *rows) {
            if (!row.conflicted)
                continue;
            bool more = true;
            while (more) {
                auto step = impl.write_locked<WriteContext::ConflictStep>(
                    [&](WriteContext& context) { return context.resolve_conflict_step(row.id, merger); },
                    true);
                if (!step) {
                    result.unresolved.push_back(
                        Error{Error::Code::Io,
                              "sync.conflict_unresolved",
                              {{"path", row.info.path.string()}, {"detail", format_error(step.error())}}});
                    break;
                }
                if (step->copy)
                    result.conflict_copies.push_back(std::move(*step->copy));
                more = step->more;
            }
        }
    impl.snapshot.reset();
    return result;
}

std::expected<graph::ExportReport, Error> export_note_log(NoteStore& store,
                                                          const std::filesystem::path& directory) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    if (auto opened = impl.open_for_write(); !opened)
        return std::unexpected(opened.error());
    auto exported = impl.opened_graph().export_log(directory);
    if (!exported)
        return std::unexpected(from_graph(exported.error(), path_utf8(directory)));
    return *exported;
}

std::expected<std::string, Error> own_log_name(NoteStore& store) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    if (auto opened = impl.open_for_write(); !opened)
        return std::unexpected(opened.error());
    return hex_of(impl.opened_graph().device()) + ".oplog";
}

std::expected<int, Error> count_local_changes(NoteStore& store, std::uint64_t since) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    const auto snapshot = impl.current();
    if (!snapshot)
        return 0;
    int changed = 0;
    for (const std::vector<NoteRow>* rows : {&snapshot->live, &snapshot->trash})
        for (const NoteRow& row : *rows)
            changed += row.info.local_seq > since ? 1 : 0;
    return changed;
}

std::expected<std::uint64_t, Error> own_last_seq(NoteStore& store) {
    NoteStore::Impl& impl = store.impl();
    const std::scoped_lock lock(impl.mutex);
    if (auto opened = impl.open_for_write(); !opened)
        return std::unexpected(opened.error());
    auto seq = impl.opened_graph().last_seq();
    if (!seq)
        return std::unexpected(from_graph(seq.error(), path_utf8(impl.database())));
    return *seq;
}

}  // namespace noteapp::core
