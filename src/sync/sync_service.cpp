#include "sync/sync_service.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "core/note_sync.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "graph/graph.hpp"
#include "sync/auth.hpp"
#include "sync/git_free.hpp"
#include "sync/lock.hpp"
#include "sync/text_merge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <git2.h>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace noteapp::sync {

namespace fs = std::filesystem;

namespace {

constexpr const char* kRemoteName = "origin";
constexpr std::string_view kLogDir = "graph";  // <notes_dir>/graph/<cihaz>.oplog (SPEC-graph)
constexpr std::size_t kLogHexBytes = 32;
constexpr std::size_t kLogNameBytes = kLogHexBytes + std::string_view(".oplog").size();
constexpr std::size_t kMaxReportedIssues = 20;  // durum dosyası büyümesin; kalanı sayı olarak bildirilir
constexpr std::string_view kBranchPrefix = "refs/heads/";
constexpr std::string_view kPushRejected = "sync.push_rejected";
constexpr std::size_t kMaxExtensionBytes = 32;  // bundan uzunu uzantı sayılmaz: kopyanın adını doldururdu
// OS kullanıcı/makine adı okunamazsa yazarda boş alan kalmasın diye son çare değerler.
constexpr std::string_view kUnknownUser = "noteapp";
constexpr std::string_view kUnknownHost = "localhost";

using Repository = std::unique_ptr<git_repository, GitFree<git_repository_free>>;
using Remote = std::unique_ptr<git_remote, GitFree<git_remote_free>>;
using Index = std::unique_ptr<git_index, GitFree<git_index_free>>;
using Tree = std::unique_ptr<git_tree, GitFree<git_tree_free>>;
using Commit = std::unique_ptr<git_commit, GitFree<git_commit_free>>;
using Reference = std::unique_ptr<git_reference, GitFree<git_reference_free>>;
using AnnotatedCommit = std::unique_ptr<git_annotated_commit, GitFree<git_annotated_commit_free>>;
using Diff = std::unique_ptr<git_diff, GitFree<git_diff_free>>;
using Signature = std::unique_ptr<git_signature, GitFree<git_signature_free>>;
using GitConfig = std::unique_ptr<git_config, GitFree<git_config_free>>;
using Blob = std::unique_ptr<git_blob, GitFree<git_blob_free>>;
using Object = std::unique_ptr<git_object, GitFree<git_object_free>>;
using ConflictIterator =
    std::unique_ptr<git_index_conflict_iterator, GitFree<git_index_conflict_iterator_free>>;

core::Error sync_error(std::string key, core::Args args = {}) {
    return {core::Error::Code::Io, std::move(key), std::move(args)};
}

core::Error git_failure() {
    return sync_error("sync.git_failed", {{"detail", git_error_last()->message}});
}

core::Error read_failure(const fs::path& file, const std::error_code& ec);

// libgit2'nin her çağrısı int döner (< 0: hata, ayrıntı git_error_last()'ta); C API'sini okunur tutar.
#define TRY_GIT(expr)                                                                                        \
    do {                                                                                                     \
        if ((expr) < 0)                                                                                      \
            return std::unexpected(git_failure());                                                           \
    } while (false)

std::string local_ref(const std::string& branch) {
    return std::string(kBranchPrefix) + branch;
}

std::string tracking_ref(const std::string& branch) {
    return std::format("refs/remotes/{}/{}", kRemoteName, branch);
}

// libgit2 Windows'ta da UTF-8 yol bekler; path::string()'in dar kodlaması platforma göre değişir.
std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path path_from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string iso_utc(std::chrono::system_clock::time_point when) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(when));
}

// Çakışma kopyası adındaki damga, yerel saatle "YYYY-MM-DD HHMM": kullanıcı adı kendi saatiyle okur.
std::string local_minute_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

std::optional<std::chrono::system_clock::time_point> parse_iso_utc(const std::string& text) {
    int year = 0;
    unsigned month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    char zone = 0;
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion) -- alan sayısı aşağıda denetlenir
    if (std::sscanf(text.c_str(), "%d-%u-%uT%d:%d:%d%c", &year, &month, &day, &hour, &minute, &second,
                    &zone) != 7 ||
        zone != 'Z')
        return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(month),
                                           std::chrono::day(day)};
    if (!date.ok())
        return std::nullopt;
    return std::chrono::sys_days(date) + std::chrono::hours(hour) + std::chrono::minutes(minute) +
           std::chrono::seconds(second);
}

std::string os_user() {
#ifdef _WIN32
    std::string user = core::get_env_utf8("USERNAME").value_or("");
#else
    std::string user = core::get_env_utf8("USER").value_or("");
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- süreçte getpw* çağıran tek yer; sonuç hemen kopyalanır
    if (const passwd* entry = getpwuid(getuid()); user.empty() && entry != nullptr)
        user = entry->pw_name;
#endif
    return user.empty() ? std::string(kUnknownUser) : user;
}

std::string host_name() {
#ifdef _WIN32
    std::string host = core::get_env_utf8("COMPUTERNAME").value_or("");
#else
    char buffer[256] = {};
    std::string host = gethostname(buffer, sizeof buffer - 1) == 0 ? buffer : "";
#endif
    return host.empty() ? std::string(kUnknownHost) : host;
}

// Boş kalan alanları dosyadaki user.name/user.email ile doldurur; dosya yoksa libgit2 boş config açar.
void fill_from_git_config(const fs::path& file, Author& author) {
    git_config* raw = nullptr;
    if (git_config_open_ondisk(&raw, utf8(file).c_str()) < 0) {
        core::log_warn("sync: cannot read " + utf8(file) + ": " + git_error_last()->message);
        return;
    }
    const GitConfig config(raw);
    for (auto [key, field] : {std::pair{"user.name", &author.name}, std::pair{"user.email", &author.email}}) {
        git_buf value = GIT_BUF_INIT;
        if (field->empty() && git_config_get_string_buf(&value, config.get(), key) == 0)
            *field = value.ptr;
        git_buf_dispose(&value);
    }
}

// Kullanıcıya söylenen sayı not değişikliğidir: `.trash/` altı sayılmaz. Yeniden ad algılama seçilmedi: çöpe
// atma silme + ekleme olarak görünür ve boş ya da benzer notlarda benzerlik eşiği sayıyı oynatırdı; bu
// kuralla çöpe atma ve geri alma her zaman 1, kalıcı silme 0'dır. Ağaç farkı ayrıca tutulur: sayılmayan
// değişiklik de commit'lenir.
struct TreeChanges {
    std::size_t deltas = 0;
    int notes = 0;
};

constexpr std::string_view kTrashPrefix = ".trash/";

std::expected<TreeChanges, core::Error> count_changes(git_repository* repo, git_tree* from, git_tree* to) {
    git_diff* raw = nullptr;
    TRY_GIT(git_diff_tree_to_tree(&raw, repo, from, to, nullptr));
    const Diff diff(raw);
    TreeChanges changes;
    changes.deltas = git_diff_num_deltas(diff.get());
    for (std::size_t i = 0; i < changes.deltas; ++i) {
        const git_diff_delta* delta = git_diff_get_delta(diff.get(), i);
        const std::string_view path =
            delta->status == GIT_DELTA_DELETED ? delta->old_file.path : delta->new_file.path;
        if (!path.starts_with(kTrashPrefix))
            ++changes.notes;
    }
    return changes;
}

// Dal ucunun ağacı; dal doğmamışsa (hiç commit yok) boş işaretçiler.
std::expected<std::pair<Commit, Tree>, core::Error> branch_tip(git_repository* repo, const std::string& ref) {
    git_oid id;
    if (const int rc = git_reference_name_to_id(&id, repo, ref.c_str()); rc == GIT_ENOTFOUND)
        return std::pair<Commit, Tree>{};
    else if (rc < 0)
        return std::unexpected(git_failure());
    git_commit* commit = nullptr;
    TRY_GIT(git_commit_lookup(&commit, repo, &id));
    Commit tip(commit);
    git_tree* tree = nullptr;
    TRY_GIT(git_commit_tree(&tree, tip.get()));
    return std::pair{std::move(tip), Tree(tree)};
}

// Commit ve hızlı ileri sarma yalnız senkron dalında: başka dala geçilmişse ona yazıp `branch`'i taşımak
// çalışma ağacıyla dalı birbirinden koparırdı.
std::expected<void, core::Error> check_head(git_repository* repo, const std::string& branch) {
    git_reference* raw = nullptr;
    TRY_GIT(git_reference_lookup(&raw, repo, "HEAD"));
    const Reference head(raw);
    const std::string target = git_reference_type(head.get()) == GIT_REFERENCE_SYMBOLIC
                                   ? git_reference_symbolic_target(head.get())
                                   : "HEAD";
    if (target != local_ref(branch)) {
        const std::string current =
            target.starts_with(kBranchPrefix) ? target.substr(kBranchPrefix.size()) : target;
        return std::unexpected(
            sync_error("sync.branch_mismatch", {{"branch", branch}, {"current", current}}));
    }
    return {};
}

std::expected<Signature, core::Error> signature_now(const core::SyncConfig& sync) {
    const Author author = resolve_author(sync);
    git_signature* raw = nullptr;
    TRY_GIT(git_signature_now(&raw, author.name.c_str(), author.email.c_str()));
    return Signature(raw);
}

// Bu cihazın ve diğerlerinin günlükleri; yalnız `<32 hex>.oplog` adı taşıyıcıdır. Dizin bağsa kullanılmaz:
// core.symlinks kapalı olduğundan git'ten gelmez, elle kurulmuş olabilir ve dışarıdaki dosyaları günlük gibi
// okuturdu.
std::expected<std::vector<fs::path>, core::Error> log_files(const fs::path& notes_dir) {
    const fs::path dir = notes_dir / kLogDir;
    std::vector<fs::path> files;
    const auto dir_stat = core::stat_file(dir);
    if (!dir_stat)
        return files;
    if (dir_stat->symlink || !dir_stat->directory)
        return std::unexpected(sync_error("sync.log_dir_not_regular", {{"path", utf8(dir)}}));
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        const bool is_log = name.size() == kLogNameBytes && name.ends_with(".oplog") &&
                            std::all_of(name.begin(), name.begin() + kLogHexBytes, [](char c) {
                                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                            });
        if (is_log)
            files.push_back(it->path());
    }
    if (ec)
        return std::unexpected(read_failure(dir, ec));
    std::ranges::sort(files);
    return files;
}

// Döngü 2: yalnız taşıyıcılar sahnelenir (.gitignore, .gitattributes, graph/*.oplog). Depodaki diğer
// dosyalara, eski notlar dahil, dokunulmaz (SPEC-sync). Ağaç değiştiyse commit; dönüş: commit atıldı mı.
std::expected<bool, core::Error> commit_carriers(git_repository* repo, const fs::path& notes_dir,
                                                 const std::string& branch, const core::SyncConfig& sync,
                                                 int note_count) {
    git_index* index_raw = nullptr;
    TRY_GIT(git_repository_index(&index_raw, repo));
    const Index index(index_raw);
    for (const char* name : {".gitignore", ".gitattributes"}) {
        std::error_code ec;
        if (fs::exists(notes_dir / name, ec))
            TRY_GIT(git_index_add_bypath(index.get(), name));
    }
    const auto logs = log_files(notes_dir);
    if (!logs)
        return std::unexpected(logs.error());
    for (const fs::path& log : *logs) {
        const std::string relative = std::string(kLogDir) + "/" + utf8(log.filename());
        TRY_GIT(git_index_add_bypath(index.get(), relative.c_str()));
    }
    TRY_GIT(git_index_write(index.get()));
    git_oid tree_id;
    TRY_GIT(git_index_write_tree(&tree_id, index.get()));
    git_tree* tree_raw = nullptr;
    TRY_GIT(git_tree_lookup(&tree_raw, repo, &tree_id));
    const Tree tree(tree_raw);

    auto parent = branch_tip(repo, local_ref(branch));
    if (!parent)
        return std::unexpected(parent.error());
    const auto changed = count_changes(repo, parent->second.get(), tree.get());
    if (!changed)
        return std::unexpected(changed.error());
    if (changed->deltas == 0)
        return false;

    const auto signature = signature_now(sync);
    if (!signature)
        return std::unexpected(signature.error());
    const std::string message =
        core::tr("sync.commit_message", {{"count", std::to_string(note_count)},
                                         {"host", host_name()},
                                         {"time", iso_utc(std::chrono::system_clock::now())}});
    const git_commit* parents[] = {parent->first.get()};
    git_oid commit_id;
    TRY_GIT(git_commit_create(&commit_id, repo, "HEAD", signature->get(), signature->get(), nullptr,
                              message.c_str(), tree.get(), parent->first ? 1 : 0, parents));
    return true;
}

// Bir ağ işleminin (ls, fetch, push) callback yükü. Kimliğin tek deneme kuralı işlem başınadır: her işlem
// hazırlanmış kimliğin taze bir kopyasıyla başlar. Push'ta sunucunun ref reddi de buraya düşer.
struct Exchange {
    Credentials credentials;
    std::string rejection;
};

int give_credentials(git_credential** out, const char* url, const char* /*username_from_url*/,
                     unsigned int allowed_types, void* payload) {
    return acquire_credentials(out, url, allowed_types, static_cast<Exchange*>(payload)->credentials);
}

int record_rejection(const char* /*refname*/, const char* status, void* payload) {
    if (status != nullptr)
        static_cast<Exchange*>(payload)->rejection = status;
    return 0;
}

void attach(git_remote_callbacks& callbacks, Exchange& exchange) {
    callbacks.credentials = give_credentials;
    callbacks.push_update_reference = record_rejection;
    callbacks.payload = &exchange;
}

// Callback kimliği reddettiyse onun nedeni (sync.auth_failed), değilse libgit2'ninki.
core::Error network_failure(const Exchange& exchange) {
    return exchange.credentials.error ? *exchange.credentials.error : git_failure();
}

std::expected<void, core::Error> fetch(git_remote* remote, const std::string& branch,
                                       const Credentials& credentials) {
    // '+' yalnız yerel takip dalını zorla günceller; uzak geçmişe yazılmaz.
    std::string refspec = "+" + local_ref(branch) + ":" + tracking_ref(branch);
    char* refspecs_raw[] = {refspec.data()};
    const git_strarray refspecs{refspecs_raw, 1};
    git_fetch_options options;
    TRY_GIT(git_fetch_options_init(&options, GIT_FETCH_OPTIONS_VERSION));
    options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    Exchange exchange{credentials, {}};
    attach(options.callbacks, exchange);
    if (git_remote_fetch(remote, &refspecs, &options, nullptr) < 0)
        return std::unexpected(network_failure(exchange));
    return {};
}

// Bu birleştirme denemesinin yazdığı çakışma kopyası: deneme geri alınırsa (bkz. abort_merge) silinir.
struct CreatedCopy {
    std::string path;  // depo-göreli
    git_oid blob;      // yazılan içerik
};

// En çok `max_bytes` bayta keser; çok baytlı bir UTF-8 karakterini bölmez.
std::string cut_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes)
        return text;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)  // 10xxxxxx: devam baytı
        --end;
    return text.substr(0, end);
}

// Onların sürümü `<ad> (conflict YYYY-MM-DD HHMM).<uzantı>` olarak yanına yazılır; ad doluysa uzantıdan önce
// " 2", " 3" … eklenir; ad 255 bayta sığmıyorsa `<ad>` kısaltılır. Dosya yalnız yoksa oluşturulur (O_EXCL):
// var olan hiçbir şeyin üzerine yazılmaz. Dönüş: depo-göreli, '/' ayırıcılı UTF-8 yol.
std::expected<std::string, core::Error> write_conflict_copy(git_repository* repo, const fs::path& workdir,
                                                            const std::string& path, const git_oid& blob_id,
                                                            const std::string& stamp) {
    git_blob* raw = nullptr;
    TRY_GIT(git_blob_lookup(&raw, repo, &blob_id));
    const Blob blob(raw);
    const std::string_view content(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                                   static_cast<std::size_t>(git_blob_rawsize(blob.get())));
    const fs::path original = path_from_utf8(path);
    std::string stem = utf8(original.stem());
    std::string extension = utf8(original.extension());  // uzantısız dosyada boş: nokta eklenmez
    if (extension.size() > kMaxExtensionBytes) {         // gerçek uzantı değil; damgaya yer kalsın
        stem += extension;
        extension.clear();
    }
    for (int number = 1;; ++number) {
        const std::string suffix = number == 1 ? "" : " " + std::to_string(number);
        const std::string tail = std::format(" (conflict {}){}{}", stamp, suffix, extension);
        const fs::path name = original.parent_path() /
                              path_from_utf8(cut_utf8(stem, core::kMaxFileNameBytes - tail.size()) + tail);
        const core::FileWriteResult written = core::write_new_file(workdir / name, content);
        if (!written.already_exists && !written.ec)
            return utf8(name);
        if (!written.already_exists) {
            std::error_code ec;
            fs::remove(workdir / name, ec);  // yarım yazılmış kendi kopyamız; içerik git'te duruyor
            return std::unexpected(
                sync_error("error.io", {{"path", utf8(workdir / name)}, {"detail", written.ec.message()}}));
        }
    }
}

// Çakışma çözümü. Değişmez kural: hiçbir taraf kaybolmaz — iki tarafın içeriği de ya kendi yolunda ya da
// yanındaki çakışma kopyasında durur; hiçbir dosya silinmez, hiçbir dosyanın üzerine yazılmaz.
//  - değiştirme/değiştirme, ekleme/ekleme: yolda bizimki (merge checkout'u USE_OURS ile yazdı), onlarınki
//    yeni bir "(conflict …)" kopyasında;
//  - silme/değiştirme (iki yönde de): değiştirilmiş sürüm yolunda (checkout tek kalan tarafı yazdı);
//  - iki tarafta aynı içerik çakışma değildir: kopya yok.
// Her yol ve kopya diskten sahnelenir; sahneleme çakışma kaydını kapatır. Dönüş: oluşturulan kopyalar (not
// olanlar); yazılan her kopya, hata olsa da, `created`'a eklenir.
std::expected<std::vector<core::NotePath>, core::Error> resolve_conflicts(git_repository* repo,
                                                                          git_index* index,
                                                                          std::vector<CreatedCopy>& created,
                                                                          const SyncTestHooks& hooks) {
    struct Conflict {
        std::string path;
        std::optional<git_oid> theirs_to_copy;
    };
    std::vector<Conflict> conflicts;  // önce topla: index değişince yineleyici geçersizleşir
    git_index_conflict_iterator* iterator_raw = nullptr;
    TRY_GIT(git_index_conflict_iterator_new(&iterator_raw, index));
    const ConflictIterator iterator(iterator_raw);
    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    int rc = 0;
    while ((rc = git_index_conflict_next(&ancestor, &ours, &theirs, iterator.get())) == 0) {
        const git_index_entry* present = ours != nullptr ? ours : theirs != nullptr ? theirs : ancestor;
        const bool both_differ =
            ours != nullptr && theirs != nullptr && !git_oid_equal(&ours->id, &theirs->id);
        conflicts.push_back({present->path, both_differ ? std::optional(theirs->id) : std::nullopt});
    }
    if (rc != GIT_ITEROVER)
        return std::unexpected(git_failure());

    const fs::path workdir = path_from_utf8(git_repository_workdir(repo));
    const std::string stamp = local_minute_stamp();
    std::vector<core::NotePath> copies;
    for (const Conflict& conflict : conflicts) {
        if (conflict.theirs_to_copy) {
            const auto copy =
                write_conflict_copy(repo, workdir, conflict.path, *conflict.theirs_to_copy, stamp);
            if (!copy)
                return std::unexpected(copy.error());
            created.push_back({*copy, *conflict.theirs_to_copy});
            if (hooks.after_conflict_copy)
                if (auto injected = hooks.after_conflict_copy(*copy); !injected)
                    return std::unexpected(injected.error());
            TRY_GIT(git_index_add_bypath(index, copy->c_str()));
            core::log_warn(
                std::format("sync: conflict in {}; remote version kept as {}", conflict.path, *copy));
            if (auto note = core::NotePath::parse(*copy))
                copies.push_back(std::move(*note));  // gizli dosyanın kopyası not değildir; diskte yine durur
        }
        // Silme/değiştirme dahil: yoldaki dosya (tek kalan sürüm ya da bizimki) sahnelenir.
        TRY_GIT(git_index_add_bypath(index, conflict.path.c_str()));
    }
    return copies;
}

// Merge'ün ikinci yarısı (git_merge'den sonra): çakışma çözümü → iki ebeveynli commit → durum temizliği.
// Dönüş: `ours_tree`'ye göre değişen dosya sayısı.
std::expected<int, core::Error> commit_merge(git_repository* repo, const git_commit* ours,
                                             git_tree* ours_tree, const git_oid* their_id,
                                             const core::SyncConfig& sync,
                                             std::vector<core::NotePath>& copies,
                                             std::vector<CreatedCopy>& created, const SyncTestHooks& hooks) {
    git_index* index_raw = nullptr;
    TRY_GIT(git_repository_index(&index_raw, repo));
    const Index index(index_raw);
    auto resolved = resolve_conflicts(repo, index.get(), created, hooks);
    if (!resolved)
        return std::unexpected(resolved.error());
    TRY_GIT(git_index_write(index.get()));
    git_oid tree_id;
    TRY_GIT(git_index_write_tree(&tree_id, index.get()));  // çözülmemiş çakışma kalırsa GIT_EUNMERGED
    git_tree* tree_raw = nullptr;
    TRY_GIT(git_tree_lookup(&tree_raw, repo, &tree_id));
    const Tree tree(tree_raw);
    git_commit* their_raw = nullptr;
    TRY_GIT(git_commit_lookup(&their_raw, repo, their_id));
    const Commit their_commit(their_raw);

    const auto signature = signature_now(sync);
    if (!signature)
        return std::unexpected(signature.error());
    const std::string message =
        core::tr("sync.merge_message", {{"host", host_name()},
                                        {"time", iso_utc(std::chrono::system_clock::now())},
                                        {"conflicts", std::to_string(resolved->size())}});
    const git_commit* parents[] = {ours, their_commit.get()};
    git_oid commit_id;
    TRY_GIT(git_commit_create(&commit_id, repo, "HEAD", signature->get(), signature->get(), nullptr,
                              message.c_str(), tree.get(), 2, parents));
    created.clear();  // kopyalar artık HEAD'de: bundan sonraki bir hatanın geri alması onları silmesin
    TRY_GIT(git_repository_state_cleanup(repo));

    copies.insert(copies.end(), resolved->begin(), resolved->end());
    const auto changed = count_changes(repo, ours_tree, tree.get());
    if (!changed)
        return std::unexpected(changed.error());
    return changed->notes;
}

// Yarım kalmış merge'ü geri alır. Hiçbir içerik kaybolmaz: merge'e başlamadan önce tüm yerel değişiklikler
// commit'lendi, HEAD ve uzak dal her baytı tutar. Asla HARD reset ya da FORCE checkout yok:
//  1. index diskteki merge index'idir (bellekteki yarım çözüm atılır); çakışma kayıtları çıkarılır — libgit2
//     çakışmalı index'le SAFE checkout yapmaz;
//  2. HEAD'e SAFE checkout, taban bu index: içeriği hâlâ merge'ün yazdığı gibi olan dosya HEAD'e döner
//     (merge'ün eklediği silinir, sildiği geri gelir). Kullanıcının arada düzenlediği dosya çakışma sayılıp
//     atlanır (ALLOW_CONFLICTS): olduğu gibi kalır, sonraki senkron commit'ler;
//  3. index HEAD'e (MIXED; libgit2 merge durum dosyalarını da siler);
//  4. bu denemenin yazdığı kopyalar, içerikleri hâlâ yazıldığı gibiyse silinir.
std::expected<void, core::Error> abort_merge(git_repository* repo, const std::vector<CreatedCopy>& created) {
    git_index* index_raw = nullptr;
    TRY_GIT(git_repository_index(&index_raw, repo));
    const Index index(index_raw);
    TRY_GIT(git_index_read(index.get(), true));
    TRY_GIT(git_index_conflict_cleanup(index.get()));
    TRY_GIT(git_index_write(index.get()));

    git_checkout_options checkout;
    TRY_GIT(git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION));
    checkout.checkout_strategy =
        GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS | GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
    checkout.baseline_index = index.get();
    TRY_GIT(git_checkout_head(repo, &checkout));

    git_object* head_raw = nullptr;
    TRY_GIT(git_revparse_single(&head_raw, repo, "HEAD"));
    const Object head(head_raw);
    TRY_GIT(git_reset(repo, head.get(), GIT_RESET_MIXED, nullptr));

    const fs::path workdir = path_from_utf8(git_repository_workdir(repo));
    for (const CreatedCopy& copy : created) {
        const fs::path file = workdir / path_from_utf8(copy.path);
        git_oid on_disk;
        if (git_odb_hashfile(&on_disk, utf8(file).c_str(), GIT_OBJECT_BLOB) < 0)
            continue;  // yok (checkout sildi) ya da okunamıyor: dokunulmaz
        std::error_code ec;
        if (!git_oid_equal(&on_disk, &copy.blob))
            core::log_warn("sync: conflict copy changed meanwhile, kept: " + copy.path);
        else if (!fs::remove(file, ec) && ec)
            core::log_error("sync: cannot remove " + copy.path + ": " + ec.message());
    }
    return {};
}

// Checkout hatası: uzak ağaçta bu işletim sisteminde yazılamayan bir yol varsa (Windows'ta aux.md, sonu
// noktalı ad; libgit2 protectNTFS) kullanıcının yapabileceği bir şey söyleyen hata, yoksa genel git hatası.
// libgit2 yolu yalnız ileti içinde verir.
core::Error checkout_failure() {
    const git_error* last = git_error_last();
    constexpr std::string_view kInvalidPath = "invalid path '";
    const std::string_view message = last->message;
    const auto start = message.find(kInvalidPath);
    if (last->klass != GIT_ERROR_CHECKOUT || start == std::string_view::npos)
        return git_failure();
    const std::string_view rest = message.substr(start + kInvalidPath.size());
    return sync_error("sync.remote_invalid_paths", {{"path", std::string(rest.substr(0, rest.rfind('\'')))}});
}

// Yolun harf büyüklüğünden bağımsız anahtarı.
fs::path::string_type fold_case(const std::string& path) {
    fs::path::string_type text = path_from_utf8(path).native();
#ifdef _WIN32
    // Değişmez yerel ayar NTFS'in harf tablosuna en yakınıdır (Türkçe i/İ kuralı yok; Ç/ç eşlenir).
    const int size = static_cast<int>(text.size());
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), size, text.data(), size, nullptr,
                  nullptr, 0);
#else
    // ponytail: yalnız ASCII (duyarsız disk burada yalnız macOS); gerekirse CFStringFold.
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return text;
}

// Büyük/küçük harf duyarsız diskte yalnız harf büyüklüğüyle ayrılan iki yol tek dosyaya düşer: checkout hata
// vermez, biri sessizce kaybolur, sonraki commit onu diğerinin içeriğiyle ezer. Uzak ağaç baştan reddedilir.
std::expected<void, core::Error> check_case_collisions(git_repository* repo,
                                                       const git_annotated_commit* their) {
    git_config* config_raw = nullptr;
    TRY_GIT(git_repository_config(&config_raw, repo));
    const GitConfig config(config_raw);
    if (int ignore_case = 0;
        git_config_get_bool(&ignore_case, config.get(), "core.ignorecase") < 0 || !ignore_case)
        return {};

    git_commit* commit_raw = nullptr;
    TRY_GIT(git_commit_lookup(&commit_raw, repo, git_annotated_commit_id(their)));
    const Commit commit(commit_raw);
    git_tree* tree_raw = nullptr;
    TRY_GIT(git_commit_tree(&tree_raw, commit.get()));
    const Tree tree(tree_raw);
    struct Walk {
        std::set<fs::path::string_type> seen;
        std::string collision;
    } walk;
    const auto visit = [](const char* root, const git_tree_entry* entry, void* payload) -> int {
        if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
            return 0;
        auto& state = *static_cast<Walk*>(payload);
        const std::string path = std::string(root) + git_tree_entry_name(entry);
        if (!state.seen.insert(fold_case(path)).second && state.collision.empty())
            state.collision = path;
        return 0;
    };
    TRY_GIT(git_tree_walk(tree.get(), GIT_TREEWALK_PRE, visit, &walk));
    if (!walk.collision.empty())
        return std::unexpected(sync_error("sync.remote_invalid_paths", {{"path", walk.collision}}));
    return {};
}

// Ayrışmış geçmiş: üç yollu merge → çakışma çözümü → iki ebeveynli commit. Ortak ata yoksa (ilgisiz geçmiş)
// hiçbir şeye dokunmadan hata: ilk kurulum enable'ın işi. Dönüş: değişen dosya sayısı.
std::expected<int, core::Error> merge_diverged(git_repository* repo, const std::string& branch,
                                               const git_annotated_commit* their,
                                               const core::SyncConfig& sync,
                                               std::vector<core::NotePath>& copies,
                                               const SyncTestHooks& hooks) {
    const auto ours = branch_tip(repo, local_ref(branch));
    if (!ours)
        return std::unexpected(ours.error());
    const git_oid* their_id = git_annotated_commit_id(their);
    git_oid base;
    if (const int rc = git_merge_base(&base, repo, git_commit_id(ours->first.get()), their_id);
        rc == GIT_ENOTFOUND)
        return std::unexpected(sync_error("sync.unrelated_histories", {{"branch", branch}}));
    else if (rc < 0)
        return std::unexpected(git_failure());

    git_merge_options merge_options;
    TRY_GIT(git_merge_options_init(&merge_options, GIT_MERGE_OPTIONS_VERSION));
    // Yeniden adlandırma tespiti kapalı: açıkken libgit2 farklı yollardaki girdileri tek çakışmada
    // birleştirir ve checkout bir tarafı yazmaz. Kapalıyken "adlandırma + düzenleme" iki dosya bırakır —
    // fazlalık, kayıp değil.
    merge_options.flags = 0;
    merge_options.file_favor = GIT_MERGE_FILE_FAVOR_NORMAL;
    // SAFE: commit'lenmemiş değişikliğin üzerine yazmak yerine merge hiç başlamaz. USE_OURS: çakışan dosyaya
    // işaretli (<<<<<<<) metin yerine bizim sürüm yazılır; diskte hiçbir an bozuk not durmaz.
    git_checkout_options checkout;
    TRY_GIT(git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION));
    checkout.checkout_strategy =
        GIT_CHECKOUT_SAFE | GIT_CHECKOUT_USE_OURS | GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
    const git_annotated_commit* heads[] = {their};
    if (git_merge(repo, heads, 1, &merge_options, &checkout) < 0)  // hata verirse kendi durumunu temizler
        return std::unexpected(checkout_failure());

    // Bundan sonraki bir hata merge'ü geri alır: depo MERGE durumunda kalsaydı her senkron repo_busy_state
    // ile dururdu.
    std::vector<CreatedCopy> created;
    auto merged =
        commit_merge(repo, ours->first.get(), ours->second.get(), their_id, sync, copies, created, hooks);
    if (!merged) {
        if (auto aborted = abort_merge(repo, created); aborted)
            core::log_warn("sync: merge undone after an error, nothing lost: " +
                           core::format_error(merged.error()));
        else
            core::log_error("sync: merge could not be undone: " + core::format_error(aborted.error()));
    }
    return merged;
}

// Döngü 4: güncel → 0; hızlı ileri (doğmamış yerel dal dahil) → checkout + dal ref'i; ayrışmış →
// merge_diverged. Dönüş: değişen dosya sayısı; çakışma kopyaları `copies`'e eklenir.
std::expected<int, core::Error> integrate(git_repository* repo, const std::string& branch,
                                          const core::SyncConfig& sync, std::vector<core::NotePath>& copies,
                                          const SyncTestHooks& hooks = {}) {
    git_reference* their_ref_raw = nullptr;
    if (const int rc = git_reference_lookup(&their_ref_raw, repo, tracking_ref(branch).c_str());
        rc == GIT_ENOTFOUND)
        return 0;  // uzak dal henüz yok (boş uzak depo)
    else if (rc < 0)
        return std::unexpected(git_failure());
    const Reference their_ref(their_ref_raw);
    git_annotated_commit* their_raw = nullptr;
    TRY_GIT(git_annotated_commit_from_ref(&their_raw, repo, their_ref.get()));
    const AnnotatedCommit their(their_raw);

    git_merge_analysis_t analysis{};
    git_merge_preference_t preference{};
    const git_annotated_commit* heads[] = {their.get()};
    TRY_GIT(git_merge_analysis(&analysis, &preference, repo, heads, 1));
    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)
        return 0;
    if (auto stored = check_case_collisions(repo, their.get()); !stored)
        return std::unexpected(stored.error());
    if (!(analysis & (GIT_MERGE_ANALYSIS_FASTFORWARD | GIT_MERGE_ANALYSIS_UNBORN)))
        return merge_diverged(repo, branch, their.get(), sync, copies, hooks);

    const std::string local = local_ref(branch);
    const auto old_tip = branch_tip(repo, local);
    if (!old_tip)
        return std::unexpected(old_tip.error());
    const git_oid* target_id = git_annotated_commit_id(their.get());
    git_commit* target_raw = nullptr;
    TRY_GIT(git_commit_lookup(&target_raw, repo, target_id));
    const Commit target(target_raw);
    git_tree* target_tree_raw = nullptr;
    TRY_GIT(git_commit_tree(&target_tree_raw, target.get()));
    const Tree target_tree(target_tree_raw);
    const auto changed = count_changes(repo, old_tip->second.get(), target_tree.get());
    if (!changed)
        return std::unexpected(changed.error());

    // SAFE: yerel değişikliğin üzerine yazmak yerine hata verir (ağaç az önce commit'lendi, olmamalı);
    // yok sayılan dosyalar da korunur. Önce checkout, sonra ref: checkout yarıda kalırsa dal eski commit'te
    // kalır, yarım güncelleme sonraki turda yerel değişiklik olarak commit'lenir — kayıp yok.
    git_checkout_options checkout;
    TRY_GIT(git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION));
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
    if (git_checkout_tree(repo, reinterpret_cast<const git_object*>(target.get()), &checkout) < 0)
        return std::unexpected(checkout_failure());

    // Dal ref'i karşılaştır-ve-değiştir ile taşınır; HEAD dala sembolik bağlı olduğundan onunla gelir.
    git_reference* moved = nullptr;
    constexpr const char* kReflog = "noteapp sync: fast-forward";
    TRY_GIT(old_tip->first ? git_reference_create_matching(&moved, repo, local.c_str(), target_id, 1,
                                                           git_commit_id(old_tip->first.get()), kReflog)
                           : git_reference_create(&moved, repo, local.c_str(), target_id, 0, kReflog));
    git_reference_free(moved);
    return changed->notes;
}

// Döngü 5: yerel dal uzaktakinden öndeyse push. Dönüş: push yapıldı mı. Ret "sync.push_rejected".
std::expected<bool, core::Error> push_if_ahead(git_repository* repo, git_remote* remote,
                                               const std::string& branch, const Credentials& credentials) {
    const std::string local = local_ref(branch);
    git_oid local_id;
    if (const int rc = git_reference_name_to_id(&local_id, repo, local.c_str()); rc == GIT_ENOTFOUND)
        return false;  // hiç commit yok
    else if (rc < 0)
        return std::unexpected(git_failure());
    git_oid remote_id;
    if (const int rc = git_reference_name_to_id(&remote_id, repo, tracking_ref(branch).c_str()); rc == 0) {
        std::size_t ahead = 0, behind = 0;
        TRY_GIT(git_graph_ahead_behind(&ahead, &behind, repo, &local_id, &remote_id));
        if (ahead == 0)
            return false;
    } else if (rc != GIT_ENOTFOUND) {
        return std::unexpected(git_failure());
    }

    std::string refspec = local + ":" + local;  // '+' yok: asla force push
    char* refspecs_raw[] = {refspec.data()};
    const git_strarray refspecs{refspecs_raw, 1};
    git_push_options options;
    TRY_GIT(git_push_options_init(&options, GIT_PUSH_OPTIONS_VERSION));
    // Sunucu ref'i güncellemeyi reddederse çağrı başarılı döner, ret exchange.rejection'a düşer.
    Exchange exchange{credentials, {}};
    attach(options.callbacks, exchange);
    if (const int rc = git_remote_push(remote, &refspecs, &options); rc == GIT_ENONFASTFORWARD)
        exchange.rejection = git_error_last()->message;  // uzak dal fetch'ten sonra ilerlemiş
    else if (rc < 0)
        return std::unexpected(network_failure(exchange));
    if (!exchange.rejection.empty())
        return std::unexpected(
            sync_error(std::string(kPushRejected), {{"branch", branch}, {"detail", exchange.rejection}}));
    return true;
}

fs::path status_file() {
    return core::state_dir() / "sync-status.toml";
}

fs::path lock_file() {
    return core::state_dir() / "sync.lock";
}

// Gizli girdiler (.git, .noteapp, geçici dosyalar) not sayılmaz (SPEC.md §8); gizli olmayan boş klasör
// sayılır. Klasör yoksa boştur.
bool has_visible_entries(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (!utf8(it->path().filename()).starts_with('.'))
            return true;
    return false;
}

std::string hex_device(const std::array<std::byte, 16>& device) {
    std::string text;
    for (const std::byte value : device)
        text += std::format("{:02x}", std::to_integer<unsigned>(value));
    return text;
}

// Motorun bir günlüğü daha yeni biçimde bulduğu neden (SPEC-graph "Parçalı çerçeve" (c)).
constexpr std::string_view kLogTooNew = "log_too_new";

// Motorun raporu kullanıcıya gidecek hatalara çevrilir; yol yerine cihaz: yol iç ayrıntıdır.
void collect_issues(const graph::MergeReport& report, SyncResult& result) {
    result.log_issues.clear();
    result.heads.clear();
    const auto add = [&](core::Error issue) {
        if (result.log_issues.size() < kMaxReportedIssues)
            result.log_issues.push_back(std::move(issue));
    };
    // Daha yeni sürümün günlüğü bozuk değildir: kullanıcıya bu cihazdaki uygulamayı güncellemesi söylenir.
    const auto too_new = [&](const std::array<std::byte, 16>& device) {
        add(sync_error("sync.log_too_new", {{"device", hex_device(device)}}));
    };
    for (const graph::StalledLog& stalled : report.stalled) {
        if (stalled.reason == kLogTooNew) {
            too_new(stalled.device);
            continue;
        }
        add(sync_error("sync.log_stalled", {{"device", hex_device(stalled.device)},
                                            {"seq", std::to_string(stalled.applied_seq)},
                                            {"reason", stalled.reason}}));
    }
    for (const graph::BadFrameReport& bad : report.bad_frames) {
        if (bad.reason == kLogTooNew) {
            too_new(bad.device);
            continue;
        }
        add(sync_error(
            "sync.log_bad_frame",
            {{"device", hex_device(bad.device)}, {"seq", std::to_string(bad.seq)}, {"reason", bad.reason}}));
    }
    for (const graph::PartialTail& tail : report.partial_tails)
        add(sync_error("sync.log_partial_tail", {{"device", hex_device(tail.device)},
                                                 {"valid", std::to_string(tail.valid_bytes)},
                                                 {"total", std::to_string(tail.file_bytes)}}));
    if (report.omitted > 0)
        add(sync_error("sync.log_omitted", {{"count", std::to_string(report.omitted)}}));
    for (const graph::DeviceHead& head : report.heads)
        result.heads.push_back({hex_device(head.device), head.log_seq, head.applied_seq});
}

// Bağlar düz dosya olarak (içinde hedef yolu) çıkarılır: notlar hiçbir bağın ardında yaşamaz (SPEC-core).
// libgit2 Linux/macOS'ta init sırasında core.symlinks'i açık bırakır; uzak depodaki `.gitignore ->
// ../.ssh/id_ed25519` gerçek bağ olarak yazılır, ensure_gitignore hedefi okuyup commit'e sokardı (sızıntı).
// Elle ya da başka bir git'le kurulmuş depolar da kapsansın diye depo her açılışta denetlenir.
std::expected<void, core::Error> disable_symlinks(git_repository* repo) {
    git_config* raw = nullptr;
    TRY_GIT(git_repository_config(&raw, repo));
    const GitConfig config(raw);
    if (int value = 1; git_config_get_bool(&value, config.get(), "core.symlinks") == 0 && value == 0)
        return {};  // zaten kapalı: .git/config her senkronda yeniden yazılmasın
    TRY_GIT(git_config_set_bool(config.get(), "core.symlinks", 0));
    return {};
}

core::Error read_failure(const fs::path& file, const std::error_code& ec) {
    return sync_error("error.io", {{"path", utf8(file)}, {"detail", ec.message()}});
}

// Düz dosyanın içeriği; yoksa nullopt. Bağ ya da düz olmayan girdi okunmaz (hedefi notes_dir dışında
// olabilir): "sync.gitignore_not_regular". POSIX'te O_NOFOLLOW, denetim ile açma arasındaki yarışı da
// kapatır.
std::expected<std::optional<std::string>, core::Error> read_regular_file(const fs::path& file) {
    const auto stat = core::stat_file(file);
    if (!stat && stat.error() == std::errc::no_such_file_or_directory)
        return std::nullopt;
    if (!stat)
        return std::unexpected(read_failure(file, stat.error()));
    if (!stat->regular || stat->symlink)
        return std::unexpected(sync_error("sync.gitignore_not_regular", {{"path", utf8(file)}}));
#ifdef _WIN32
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected(read_failure(file, std::make_error_code(std::errc::io_error)));
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
#else
    const int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(errno == ELOOP
                                   ? sync_error("sync.gitignore_not_regular", {{"path", utf8(file)}})
                                   : read_failure(file, {errno, std::generic_category()}));
    std::string text;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = ::read(fd, buffer, sizeof buffer)) > 0)
        text.append(buffer, static_cast<std::size_t>(count));
    const std::error_code ec(count < 0 ? errno : 0, std::generic_category());
    ::close(fd);
    if (ec)
        return std::unexpected(read_failure(file, ec));
    return text;
#endif
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
        lines.push_back(line.ends_with('\r') ? line.substr(0, line.size() - 1) : line);
    return lines;
}

// Eksik satırlar sona eklenir; var olan içerik (CRLF dahil) olduğu gibi kalır. Okunamayan dosya boş sayılıp
// üzerine yazılmaz: kullanıcının satırlarını silerdi.
std::expected<void, core::Error> add_missing_lines(const fs::path& file,
                                                   const std::vector<std::string>& wanted) {
    auto existing = read_regular_file(file);
    if (!existing)
        return std::unexpected(existing.error());
    std::string text = existing->value_or("");
    std::vector<std::string> present = split_lines(text);
    std::string missing;
    for (const std::string& line : wanted) {
        if (line.empty() || std::ranges::find(present, line) != present.end())
            continue;
        missing += line + "\n";
        present.push_back(line);
    }
    if (missing.empty())
        return {};
    if (!text.empty() && !text.ends_with('\n'))
        text += '\n';
    return core::write_file_atomically(file, text + missing);
}

// Yönetilen dosyanın içeriği; bağ ya da okunamayan girdide boş (ensure_* zaten reddetti).
std::optional<std::string> read_if_present(const fs::path& file) {
    auto text = read_regular_file(file);
    return text ? *text : std::nullopt;
}

std::expected<void, core::Error> ensure_gitignore(const fs::path& notes_dir) {
    return add_missing_lines(notes_dir / ".gitignore", {".noteapp/", ".*.tmp", "graph/*.aside-*"});
}

// Günlükler ikilidir ve tek yazarlıdır: git satır sonu dönüştürmez, metin birleştirmez (SPEC-graph "Dosya").
std::expected<void, core::Error> ensure_gitattributes(const fs::path& notes_dir) {
    return add_missing_lines(notes_dir / ".gitattributes", {"*.oplog -text -merge"});
}

// Klonun checkout'unu engellememesi için kenara alınan .gitignore geri gelir: checkout yazmadıysa bayt bayt,
// yazdıysa eksik satırları ona eklenir.
std::expected<void, core::Error> restore_gitignore(const fs::path& file, const std::string& original) {
    const auto current = read_regular_file(file);
    if (!current)
        return std::unexpected(current.error());
    if (!*current)
        return core::write_file_atomically(file, original);
    return add_missing_lines(file, split_lines(original));
}

// Bayat kilit devralındı: önceki senkron yarıda öldürülmüş olabilir (AutoSync zaman aşımı, çökme; SPEC-sync
// karar 3). Kilit artık bizde ve notlar reposuna noteapp'ten yazan başka süreç yok: libgit2'nin bıraktığı
// `*.lock` dosyaları ölü sürecindir, kalırsa her senkron "index is locked" ile düşer. Yarım merge (çakışmalı
// olsa da) abort_merge ile geri alınır; döngü merge'ü yeniden yapar. Kalsaydı her senkron
// "sync.repo_busy_state" ile dururdu.
// ponytail: o an elle çalışan bir git komutunun kilidi de silinebilir (pencere milisaniyeler; git ikinci
// yazanı yine durdurur).
void recover_interrupted_sync(const fs::path& notes_dir) {
    const fs::path git_dir = notes_dir / ".git";
    std::vector<fs::path> locks;
    std::error_code ec;
    for (fs::directory_iterator it(git_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".lock")
            locks.push_back(it->path());
    for (fs::recursive_directory_iterator it(git_dir / "refs", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".lock")
            locks.push_back(it->path());
    for (const fs::path& lock : locks) {
        if (fs::remove(lock, ec))
            core::log_warn("sync: removed lock left by an interrupted sync: " + utf8(lock));
        else if (ec)
            core::log_error("sync: cannot remove " + utf8(lock) + ": " + ec.message());
    }

    git_repository* raw = nullptr;
    if (git_repository_open(&raw, utf8(notes_dir).c_str()) < 0)
        return;  // depo yok ya da açılamıyor: döngü kendisi raporlar
    const Repository repo(raw);
    if (git_repository_state(repo.get()) != GIT_REPOSITORY_STATE_MERGE)
        return;
    // Geri alma checkout'u HEAD'deki bağları da yazar: önce kapatılır.
    // ponytail: öldürülen denemenin yazdığı kopyalar izlenemez; kalır, sonraki commit'e fazlalık olarak
    // girer.
    auto aborted = disable_symlinks(repo.get());
    if (aborted)
        aborted = abort_merge(repo.get(), {});
    if (aborted)
        core::log_warn("sync: undid a merge left by an interrupted sync");
    else
        core::log_error("sync: cannot undo a merge left by an interrupted sync: " +
                        core::format_error(aborted.error()));
}

// libgit2 nesne dosyalarını salt-okunur yazar; Windows salt-okunur dosyayı silmez.
void remove_git_dir(const fs::path& git_dir) {
    std::error_code ec;
    std::error_code ignored;
    for (fs::recursive_directory_iterator it(git_dir, ec), end; !ec && it != end; it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ignored);
    if (fs::remove_all(git_dir, ec); ec)
        core::log_error("sync: cannot remove half-initialized " + utf8(git_dir) + ": " + ec.message());
}

// Kurulum tablosunun ilk iki satırı: .git zaten var. origin aynıysa no-op; farklı ya da yoksa onay
// (`replace_origin`) gelene kadar "sync.remote_mismatch".
std::expected<void, core::Error> adopt_repository(const fs::path& notes_dir, const std::string& remote,
                                                  bool replace_origin) {
    git_repository* repo_raw = nullptr;
    TRY_GIT(git_repository_open(&repo_raw, utf8(notes_dir).c_str()));
    const Repository repo(repo_raw);
    if (auto no_links = disable_symlinks(repo.get()); !no_links)
        return std::unexpected(no_links.error());
    std::optional<std::string> current;
    git_remote* origin_raw = nullptr;
    if (const int rc = git_remote_lookup(&origin_raw, repo.get(), kRemoteName); rc == 0) {
        const Remote origin(origin_raw);
        const char* url = git_remote_url(origin.get());
        current = url != nullptr ? url : "";
    } else if (rc != GIT_ENOTFOUND) {
        return std::unexpected(git_failure());
    }
    if (current == remote)
        return {};
    if (!replace_origin)
        return std::unexpected(
            sync_error("sync.remote_mismatch",
                       {{"current", without_userinfo(current.value_or(""))}, {"remote", remote}}));
    // Silme, eski uzak takip dallarını da götürür: yeni remote boşsa bayat origin/<dal> push'u "güncel" sanıp
    // engellerdi.
    if (current)
        TRY_GIT(git_remote_delete(repo.get(), kRemoteName));
    git_remote* created = nullptr;
    TRY_GIT(git_remote_create(&created, repo.get(), kRemoteName, remote.c_str()));
    git_remote_free(created);
    return {};
}

// Uzaktaki ref adları (boş uzak depoda hiçbiri). Remote depoya bağlıdır: bağımsız ("detached") remote exec
// SSH ile çöker (SPEC-sync karar 4).
std::expected<std::vector<std::string>, core::Error> remote_refs(git_remote* remote,
                                                                 const Credentials& credentials) {
    Exchange exchange{credentials, {}};
    git_remote_callbacks callbacks;
    TRY_GIT(git_remote_init_callbacks(&callbacks, GIT_REMOTE_CALLBACKS_VERSION));
    attach(callbacks, exchange);
    if (git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr) < 0)
        return std::unexpected(network_failure(exchange));
    const git_remote_head** heads = nullptr;
    std::size_t count = 0;
    const int rc = git_remote_ls(&heads, &count, remote);
    std::vector<std::string> names;
    for (std::size_t i = 0; rc == 0 && i < count; ++i)
        names.emplace_back(heads[i]->name);
    git_remote_disconnect(remote);  // `exchange` bu kapsamla ölür; bağlantı onu tutmasın
    if (rc < 0)
        return std::unexpected(git_failure());
    return names;
}

// Uzak boş: yönetilen dosyalar ve bu cihazın günlüğü ilk commit olur ve gönderilir. Geçiş bekliyorsa günlük
// verilmez: enable yine kurulur, senkron geçişe kadar reddeder.
std::expected<void, core::Error> seed_empty_remote(const core::Config& config, git_repository* repo,
                                                   git_remote* remote, const Credentials& credentials) {
    const fs::path& notes_dir = config.notes_dir;
    NOTEAPP_TRY(ensure_gitignore(notes_dir));
    NOTEAPP_TRY(ensure_gitattributes(notes_dir));
    core::NoteStore store = core::NoteStore::for_config(config);
    if (!store.health())
        NOTEAPP_TRY(core::export_note_log(store, notes_dir / kLogDir));
    NOTEAPP_TRY(commit_carriers(repo, notes_dir, config.sync.branch, config.sync, 0));
    NOTEAPP_TRY(push_if_ahead(repo, remote, config.sync.branch, credentials));
    return {};
}

// Kurulum tablosunun son üç satırı: .git yok. Depo bu fonksiyonda oluşturulur (`created`); hata olursa
// çağıran onu siler.
std::expected<void, core::Error> initialize(const core::Config& config, const std::string& remote_url,
                                            const Credentials& credentials, bool local_empty, bool& created) {
    const fs::path& notes_dir = config.notes_dir;
    const std::string& branch = config.sync.branch;
    git_repository_init_options options;
    TRY_GIT(git_repository_init_options_init(&options, GIT_REPOSITORY_INIT_OPTIONS_VERSION));
    options.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    options.initial_head = branch.c_str();  // kullanıcının init.defaultBranch ayarına bırakılmaz
    git_repository* repo_raw = nullptr;
    TRY_GIT(git_repository_init_ext(&repo_raw, utf8(notes_dir).c_str(), &options));
    created = true;
    const Repository repo(repo_raw);
    NOTEAPP_TRY(disable_symlinks(repo.get()));
    git_remote* remote_raw = nullptr;
    TRY_GIT(git_remote_create(&remote_raw, repo.get(), kRemoteName, remote_url.c_str()));
    const Remote remote(remote_raw);

    const auto refs = remote_refs(remote.get(), credentials);
    if (!refs)
        return std::unexpected(refs.error());
    if (std::ranges::none_of(*refs, [](const std::string& name) { return name.starts_with("refs/"); }))
        return seed_empty_remote(config, repo.get(), remote.get(), credentials);
    if (std::ranges::find(*refs, local_ref(branch)) == refs->end())
        return std::unexpected(sync_error("sync.remote_branch_missing", {{"branch", branch}}));
    if (!local_empty)
        return std::unexpected(sync_error("sync.both_non_empty", {{"path", utf8(notes_dir)}}));
    // Uzakta geçmiş varken fetch'ten önce commit atılmaz: atılsaydı iki ilgisiz geçmiş doğardı (T25 bulgusu).
    // Doğmamış dala hızlı ileri sarma = klon; notes_dir var ve gizli girdiler içeriyor olsa da çalışır.
    NOTEAPP_TRY(fetch(remote.get(), branch, credentials));
    // Gizli girdiler klasörü "dolu" yapmaz ama izlenmeyen bir .gitignore SAFE checkout'u durdurur. Silinmez,
    // kenara alınır (`.*.tmp`: yok sayılır): süreç tam burada öldürülse de satırlar kaybolmaz. Diğer gizli
    // dosyalardaki çakışma checkout'u temizce durdurur; hiçbirinin üzerine yazılmaz.
    const fs::path gitignore = notes_dir / ".gitignore";
    const fs::path set_aside = notes_dir / ".gitignore.noteapp.tmp";
    const auto leftover = read_regular_file(gitignore);
    if (!leftover)
        return std::unexpected(leftover.error());
    if (*leftover) {
        std::error_code ec;
        if (fs::rename(gitignore, set_aside, ec); ec)
            return std::unexpected(read_failure(gitignore, ec));
    }
    std::vector<core::NotePath> copies;  // doğmamış dalda birleştirme yok, kopya da oluşmaz
    const auto pulled = integrate(repo.get(), branch, config.sync, copies);
    if (*leftover) {
        NOTEAPP_TRY(restore_gitignore(gitignore, **leftover));  // hatada kenardaki kopya yerinde kalır
        std::error_code ec;
        fs::remove(set_aside, ec);
    }
    if (!pulled)
        return std::unexpected(pulled.error());
    NOTEAPP_TRY(ensure_gitignore(notes_dir));
    return ensure_gitattributes(notes_dir);
}

// Kilit altında kurulum: önce adres doğrulanır, sonra tabloya göre dallanılır. Ya tam kurulum ya hiç: yarım
// kalan .git ve bu çağrının yazdığı .gitignore silinir; notlara dokunulmaz (yarıda kalan bir klonun yazdığı
// dosyalar da kalır — silmek veri kaybı riskidir).
std::expected<void, core::Error> enable_locked(const core::Config& config, std::string_view remote,
                                               bool replace_origin) {
    if (auto valid = validate_remote(remote); !valid)
        return std::unexpected(valid.error());
    const fs::path& notes_dir = config.notes_dir;
    const std::string url(remote);
    std::error_code ec;
    if (fs::exists(notes_dir / ".git", ec))
        return adopt_repository(notes_dir, url, replace_origin);

    const auto credentials = prepare_credentials(url);  // ağa çıkmadan: SSH ön kontrolü / token
    if (!credentials)
        return std::unexpected(credentials.error());
    const bool local_empty = !has_visible_entries(notes_dir);
    const bool had_old_notes = core::legacy_notes_pending(notes_dir, core::data_dir());
    const bool had_gitignore = fs::exists(notes_dir / ".gitignore", ec);
    const bool had_gitattributes = fs::exists(notes_dir / ".gitattributes", ec);
    const bool had_log_dir = fs::exists(notes_dir / kLogDir, ec);
    bool created = false;
    auto outcome = initialize(config, url, *credentials, local_empty, created);
    // Klonla gelen düz dosyalar bu cihazın eski notu değildir.
    if (outcome && !had_old_notes)
        outcome = core::mark_notes_dir_current(notes_dir, core::data_dir());
    if (!outcome && created) {  // yalnız kendi oluşturduğumuz depo: başkasının .git'ine asla dokunulmaz
        remove_git_dir(notes_dir / ".git");
        if (!had_gitignore)
            fs::remove(notes_dir / ".gitignore", ec);
        if (!had_gitattributes)
            fs::remove(notes_dir / ".gitattributes", ec);
        if (!had_log_dir)  // günlükler graph.db'den yeniden üretilir: burada kayıp yok
            fs::remove_all(notes_dir / kLogDir, ec);
    }
    return outcome;
}

// Bir senkron döngüsünün ortak durumu. Adımlar SPEC-sync'teki sırayla çağrılır.
struct LogCycle {
    git_repository* repo{};
    git_remote* remote{};
    const Credentials& credentials;
    const core::Config& config;
    core::NoteStore& store;
    fs::path log_dir;
    std::uint64_t pushed_before{};
    SyncResult& result;
    std::vector<core::Error> set_aside;
    const SyncTestHooks& hooks;
};

// notes_dir deposu ve origin'i; değilse "sync.not_initialized".
std::expected<std::pair<Repository, Remote>, core::Error> open_sync_repository(const fs::path& notes) {
    const std::string notes_dir = utf8(notes);
    const core::Error not_initialized{
        core::Error::Code::NotFound, "sync.not_initialized", {{"path", notes_dir}}};
    git_repository* repo_raw = nullptr;
    if (const int rc = git_repository_open(&repo_raw, notes_dir.c_str()); rc == GIT_ENOTFOUND)
        return std::unexpected(not_initialized);
    else if (rc < 0)
        return std::unexpected(git_failure());
    Repository repo(repo_raw);
    git_remote* remote_raw = nullptr;
    if (git_repository_is_bare(repo.get()) || git_remote_lookup(&remote_raw, repo.get(), kRemoteName) < 0)
        return std::unexpected(not_initialized);
    return std::pair<Repository, Remote>{std::move(repo), Remote(remote_raw)};
}

// Sıra bağlayıcı (SPEC-graph): önce kendi günlüğü alınır. Geri yüklenmiş graph.db böyle yetişir; atlanırsa
// yeni yazma aynı seq'i farklı içerikle üretip diğer cihazlarda seq_digest_mismatch doğururdu.
std::expected<void, core::Error> ingest_own_log(LogCycle& cycle) {
    const auto name = core::own_log_name(cycle.store);
    if (!name)
        return std::unexpected(name.error());
    const std::vector<fs::path> own{cycle.log_dir / path_from_utf8(*name)};
    std::error_code ec;
    if (fs::exists(own.front(), ec))
        NOTEAPP_TRY(core::ingest_note_logs(cycle.store, own));
    return {};
}

// Doğmamış dal ve uzakta geçmiş: önce uzak alınır. Yoksa ilk commit ayrı bir kök olur, iki cihaz ilgisiz
// geçmişe düşer (T25 bulgusu, enable'ın klon yolu da bu yüzden commit'ten önce fetch eder).
std::expected<void, core::Error> pull_into_unborn_branch(LogCycle& cycle) {
    const std::string& branch = cycle.config.sync.branch;
    const fs::path& notes = cycle.config.notes_dir;
    const auto tip = branch_tip(cycle.repo, local_ref(branch));
    if (!tip)
        return std::unexpected(tip.error());
    if (tip->first)
        return {};
    NOTEAPP_TRY(fetch(cycle.remote, branch, cycle.credentials));
    // Yönetilen dosyalar checkout'u durdurmasın: ensure_* az önce yazdı, uzaktan gelen sürüme eklenir.
    const std::optional<std::string> ignore = read_if_present(notes / ".gitignore");
    const std::optional<std::string> attributes = read_if_present(notes / ".gitattributes");
    std::error_code removed;
    fs::remove(notes / ".gitignore", removed);
    fs::remove(notes / ".gitattributes", removed);
    std::vector<core::NotePath> none;
    const auto pulled = integrate(cycle.repo, branch, cycle.config.sync, none, cycle.hooks);
    if (ignore)
        NOTEAPP_TRY(restore_gitignore(notes / ".gitignore", *ignore));
    if (attributes)
        NOTEAPP_TRY(restore_gitignore(notes / ".gitattributes", *attributes));
    if (!pulled)
        return std::unexpected(pulled.error());
    return {};
}

// Günlüğü dizine yazar ve taşıyıcıları commit'ler.
std::expected<void, core::Error> publish_log(LogCycle& cycle) {
    NOTEAPP_TRY(log_files(cycle.config.notes_dir));  // uzak commit graph'ı dosyaya çevirdiyse net hata
    std::error_code ec;
    fs::create_directories(cycle.log_dir, ec);
    if (ec)
        return std::unexpected(read_failure(cycle.log_dir, ec));
    auto exported = core::export_note_log(cycle.store, cycle.log_dir);
    if (!exported)
        return std::unexpected(exported.error());
    if (exported->set_aside)
        cycle.set_aside.push_back(
            sync_error("sync.log_set_aside",
                       {{"path", utf8(exported->set_aside->filename())}, {"reason", exported->reason}}));
    const auto unsent = core::count_local_changes(cycle.store, cycle.pushed_before);
    if (!unsent)
        return std::unexpected(unsent.error());
    cycle.result.committed = *unsent;
    // publish sonrası başka süreç yazabilir: yalnız günlüğe girmiş olan gönderilmiş sayılır.
    cycle.result.exported_seq = exported->last_seq;
    NOTEAPP_TRY(commit_carriers(cycle.repo, cycle.config.notes_dir, cycle.config.sync.branch,
                                cycle.config.sync, *unsent));
    return {};
}

// Döngünün 3–5. adımları: fetch → birleştir → günlükleri al ve çöz → ver → push.
std::expected<bool, core::Error> exchange_logs(LogCycle& cycle) {
    const std::string& branch = cycle.config.sync.branch;
    NOTEAPP_TRY(fetch(cycle.remote, branch, cycle.credentials));
    const auto pulled =
        integrate(cycle.repo, branch, cycle.config.sync, cycle.result.conflict_copies, cycle.hooks);
    if (!pulled)
        return std::unexpected(pulled.error());
    const auto logs = log_files(cycle.config.notes_dir);
    if (!logs)
        return std::unexpected(logs.error());
    const std::size_t max_merge_bytes = cycle.config.limits.sync.merge_input_bytes.cap();
    auto ingested = core::ingest_note_logs(
        cycle.store, *logs,
        [max_merge_bytes](std::string_view base, std::string_view ours, std::string_view theirs) {
            return merge_text(base, ours, theirs, max_merge_bytes);
        });
    if (!ingested)
        return std::unexpected(ingested.error());
    cycle.result.pulled += ingested->changed_notes;
    cycle.result.conflict_copies.insert(cycle.result.conflict_copies.end(), ingested->conflict_copies.begin(),
                                        ingested->conflict_copies.end());
    collect_issues(ingested->report, cycle.result);
    cycle.result.log_issues.insert(cycle.result.log_issues.end(), ingested->unresolved.begin(),
                                   ingested->unresolved.end());
    for (const core::NotePath& copy : ingested->conflict_copies)
        core::log_warn("sync: body conflict; the losing version is kept as " + copy.string());
    NOTEAPP_TRY(publish_log(cycle));
    if (cycle.hooks.before_push)
        cycle.hooks.before_push();
    return push_if_ahead(cycle.repo, cycle.remote, branch, cycle.credentials);
}

}  // namespace

Author resolve_author(const core::SyncConfig& sync) {
    const GitRuntime runtime;  // git_config_open_ondisk için; iç içe örnek güvenli
    Author author{sync.author_name, sync.author_email};
    const auto home_env = core::get_env_utf8("HOME");  // git de Windows'ta önce HOME'a bakar
    const fs::path home = home_env ? path_from_utf8(*home_env) : core::home_dir();
    const auto xdg_env = core::get_env_utf8("XDG_CONFIG_HOME");
    const fs::path xdg = xdg_env ? path_from_utf8(*xdg_env) : home / ".config";
    for (const fs::path& file : {home / ".gitconfig", xdg / "git" / "config"})
        if (author.name.empty() || author.email.empty())
            fill_from_git_config(file, author);
    const std::string user = os_user();
    if (author.name.empty())
        author.name = user;
    if (author.email.empty())
        author.email = user + "@" + host_name();
    return author;
}

std::expected<bool, core::Error>
retry_rejected_push(const std::function<std::expected<bool, core::Error>()>& exchange) {
    auto result = exchange();
    if (!result && result.error().message_key == kPushRejected) {
        core::log_warn("sync: push rejected, fetching and retrying once: " +
                       core::format_error(result.error()));
        result = exchange();
    }
    return result;
}

SyncService::SyncService(core::Config config) : config_(std::move(config)) {}

std::expected<void, core::Error> SyncService::enable(std::string_view remote, bool replace_origin) {
    // Kilit: kurulum sürerken bir arka plan senkronu yarım depoda commit atıp ilgisiz geçmiş doğurmasın.
    const auto lock =
        SyncLock::acquire(lock_file(), std::chrono::minutes(config_.tuning.sync_stale_lock_minutes));
    if (!lock)
        return std::unexpected(lock.error());
    if (lock->took_over())
        recover_interrupted_sync(config_.notes_dir);
    auto outcome = enable_locked(config_, remote, replace_origin);
    if (outcome)
        core::log_info("sync: enabled with origin " + std::string(remote));  // doğrulandı: kimlik içermez
    else
        core::log_error("sync: enable: " + core::format_error(outcome.error()));
    return outcome;
}

std::expected<SyncResult, core::Error> SyncService::sync_now(const SyncTestHooks& hooks) {
    const auto lock =
        SyncLock::acquire(lock_file(), std::chrono::minutes(config_.tuning.sync_stale_lock_minutes));
    if (!lock) {
        // Durum dosyası kilidi tutan sürecindir; meşgulken yazılmaz.
        const auto log = lock.error().message_key == "sync.busy" ? core::log_info : core::log_error;
        log("sync: " + core::format_error(lock.error()));
        return std::unexpected(lock.error());
    }
    if (lock->took_over())
        recover_interrupted_sync(config_.notes_dir);
    auto outcome = run_cycle(hooks);
    record(outcome);
    return outcome;
}

void SyncService::record_failure(const core::Error& error) const {
    std::error_code ec;
    fs::create_directories(core::state_dir(), ec);  // senkron süreci hiç başlayamadıysa dizin yoktur
    record(std::unexpected(error));
}

std::expected<SyncResult, core::Error> SyncService::run_cycle(const SyncTestHooks& hooks) {
    auto opened = open_sync_repository(config_.notes_dir);
    if (!opened)
        return std::unexpected(opened.error());
    const Repository& repo = opened->first;
    const Remote& remote = opened->second;
    const std::string notes_dir = utf8(config_.notes_dir);

    // Elle başlatılmış ya da yarıda kalmış merge/rebase/cherry-pick: index'teki çakışmalar commit'e girerdi.
    // Kullanıcı bitirene (ya da iptal edene) kadar hiçbir şeye dokunulmaz.
    if (git_repository_state(repo.get()) != GIT_REPOSITORY_STATE_NONE)
        return std::unexpected(sync_error("sync.repo_busy_state", {{"path", notes_dir}}));
    // index ve checkout bu ayarı ilk kullanımda okur: hiçbir şeye dokunmadan önce.
    NOTEAPP_TRY(disable_symlinks(repo.get()));
    NOTEAPP_TRY(check_head(repo.get(), config_.sync.branch));
    // Ağa çıkmadan, hiçbir şeye dokunmadan: SSH host anahtarı ön kontrolü, HTTPS token'ı.
    const char* url = git_remote_url(remote.get());
    const auto credentials = prepare_credentials(url != nullptr ? url : "");
    if (!credentials)
        return std::unexpected(credentials.error());

    core::NoteStore store = core::NoteStore::for_config(config_);
    if (const auto issue = store.health())  // geçiş bekliyor ya da depo açılamıyor: git'e hiç dokunulmaz
        return std::unexpected(*issue);
    // Burada geçirilecek yerel eski not yok: bundan sonra git'le gelen dosya bu cihazın eski notu sayılmaz.
    NOTEAPP_TRY(core::mark_notes_dir_current(config_.notes_dir, core::data_dir()));
    NOTEAPP_TRY(ensure_gitattributes(config_.notes_dir));
    NOTEAPP_TRY(ensure_gitignore(config_.notes_dir));

    const auto own_log = core::own_log_name(store);
    if (!own_log)
        return std::unexpected(own_log.error());
    SyncResult result;
    result.pushed_log = *own_log;
    LogCycle cycle{repo.get(),
                   remote.get(),
                   *credentials,
                   config_,
                   store,
                   config_.notes_dir / kLogDir,
                   pushed_seq_of(*own_log),
                   result,
                   {},
                   hooks};
    NOTEAPP_TRY(ingest_own_log(cycle));
    NOTEAPP_TRY(pull_into_unborn_branch(cycle));
    NOTEAPP_TRY(publish_log(cycle));
    const auto pushed = retry_rejected_push([&] { return exchange_logs(cycle); });
    if (!pushed)
        return std::unexpected(pushed.error());
    result.pushed = *pushed;
    result.log_issues.insert(result.log_issues.end(), cycle.set_aside.begin(), cycle.set_aside.end());
    result.pushed_seq = result.exported_seq;
    return result;
}

void SyncService::record(const std::expected<SyncResult, core::Error>& outcome) const {
    SyncStatus status = this->status();  // önceki başarı zamanı hatada korunur
    if (outcome) {
        status.last_success = std::chrono::system_clock::now();
        status.last_error.reset();
        status.pushed_seq = status.pushed_log == outcome->pushed_log
                                ? std::max(status.pushed_seq, outcome->pushed_seq)
                                : outcome->pushed_seq;
        status.pushed_log = outcome->pushed_log;
        status.log_issues = outcome->log_issues;
        status.heads = outcome->heads;
        for (const core::Error& issue : status.log_issues)
            core::log_warn("sync: " + core::format_error(issue));
        core::log_info(std::format("sync: ok (committed {}, pulled {}, pushed {})", outcome->committed,
                                   outcome->pulled, outcome->pushed));
    } else {
        status.last_error = outcome.error();
        core::log_error("sync: " + core::format_error(outcome.error()));
    }

    toml::table table;
    if (status.last_success)
        table.insert("last_success", iso_utc(*status.last_success));
    if (status.last_error) {
        toml::table args;
        for (const auto& [name, value] : status.last_error->args)
            args.insert(name, value);
        table.insert("last_error",
                     toml::table{{"key", status.last_error->message_key}, {"args", std::move(args)}});
    }
    table.insert("pushed_seq", static_cast<std::int64_t>(status.pushed_seq));
    table.insert("pushed_log", status.pushed_log);
    toml::array issues;
    for (const core::Error& issue : status.log_issues) {
        toml::table args;
        for (const auto& [name, value] : issue.args)
            args.insert(name, value);
        issues.push_back(toml::table{{"key", issue.message_key}, {"args", std::move(args)}});
    }
    table.insert("log_issues", std::move(issues));
    toml::array heads;
    for (const DeviceLogHead& head : status.heads)
        heads.push_back(toml::table{{"device", head.device},
                                    {"log_seq", static_cast<std::int64_t>(head.log_seq)},
                                    {"applied_seq", static_cast<std::int64_t>(head.applied_seq)}});
    table.insert("heads", std::move(heads));
    std::ostringstream text;
    text << table << '\n';
    // state_dir() kilit alınırken oluşturuldu.
    if (auto written = core::write_file_atomically(status_file(), text.str()); !written)
        core::log_error("sync: " + core::format_error(written.error()));
}

std::uint64_t SyncService::pushed_seq_of(std::string_view own_log) const {
    const SyncStatus current = status();
    return current.pushed_log == own_log ? current.pushed_seq : 0;
}

SyncStatus SyncService::status() const {
    std::ifstream in(status_file(), std::ios::binary);
    if (!in)
        return {};
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        core::log_warn("sync: unreadable status file: " + std::string(parsed.error().description()));
        return {};
    }
    const toml::table& table = parsed.table();
    SyncStatus status;
    if (const auto when = table["last_success"].value<std::string>())
        status.last_success = parse_iso_utc(*when);
    if (const toml::table* error = table["last_error"].as_table()) {
        core::Error last{core::Error::Code::Io, (*error)["key"].value_or(std::string()), {}};
        if (const toml::table* args = (*error)["args"].as_table())
            for (const auto& [name, value] : *args)
                last.args.emplace_back(std::string(name.str()), value.value_or(std::string()));
        status.last_error = std::move(last);
    }
    status.pushed_seq = static_cast<std::uint64_t>(table["pushed_seq"].value_or(std::int64_t{0}));
    status.pushed_log = table["pushed_log"].value_or(std::string());
    if (const toml::array* issues = table["log_issues"].as_array())
        for (const toml::node& node : *issues)
            if (const toml::table* issue = node.as_table()) {
                core::Error parsed{core::Error::Code::Io, (*issue)["key"].value_or(std::string()), {}};
                if (const toml::table* args = (*issue)["args"].as_table())
                    for (const auto& [name, value] : *args)
                        parsed.args.emplace_back(std::string(name.str()), value.value_or(std::string()));
                status.log_issues.push_back(std::move(parsed));
            }
    if (const toml::array* heads = table["heads"].as_array())
        for (const toml::node& node : *heads)
            if (const toml::table* head = node.as_table())
                status.heads.push_back(
                    {(*head)["device"].value_or(std::string()),
                     static_cast<std::uint64_t>((*head)["log_seq"].value_or(std::int64_t{0})),
                     static_cast<std::uint64_t>((*head)["applied_seq"].value_or(std::int64_t{0}))});
    return status;
}

}  // namespace noteapp::sync
