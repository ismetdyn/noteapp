#include "sync/sync_service.hpp"

#include "core/atomic_file.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "sync/lock.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <git2.h>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace noteapp::sync {

namespace fs = std::filesystem;

namespace {

constexpr const char* kRemoteName = "origin";
constexpr std::string_view kBranchPrefix = "refs/heads/";
constexpr std::string_view kPushRejected = "sync.push_rejected";
// OS kullanıcı/makine adı okunamazsa yazarda boş alan kalmasın diye son çare değerler.
constexpr std::string_view kUnknownUser = "noteapp";
constexpr std::string_view kUnknownHost = "localhost";

template <auto Free> struct GitFree {
    template <class T> void operator()(T* ptr) const { Free(ptr); }
};
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
using ConflictIterator =
    std::unique_ptr<git_index_conflict_iterator, GitFree<git_index_conflict_iterator_free>>;

core::Error sync_error(std::string key, core::Args args = {}) {
    return {core::Error::Code::Io, std::move(key), std::move(args)};
}

core::Error git_failure() {
    return sync_error("sync.git_failed", {{"detail", git_error_last()->message}});
}

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

std::expected<int, core::Error> count_changes(git_repository* repo, git_tree* from, git_tree* to) {
    git_diff* raw = nullptr;
    TRY_GIT(git_diff_tree_to_tree(&raw, repo, from, to, nullptr));
    const Diff diff(raw);
    return static_cast<int>(git_diff_num_deltas(diff.get()));
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

// Döngü 2: eklenen, değişen ve silinen her şey (.gitignore'a uyarak) sahnelenir; ağaç değiştiyse commit.
std::expected<int, core::Error> commit_all(git_repository* repo, const std::string& branch,
                                           const core::SyncConfig& sync) {
    git_index* index_raw = nullptr;
    TRY_GIT(git_repository_index(&index_raw, repo));
    const Index index(index_raw);
    TRY_GIT(git_index_update_all(index.get(), nullptr, nullptr, nullptr));  // silinenler
    TRY_GIT(git_index_add_all(index.get(), nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr));
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
    if (!changed || *changed == 0)
        return changed;

    const auto signature = signature_now(sync);
    if (!signature)
        return std::unexpected(signature.error());
    const std::string message =
        core::tr("sync.commit_message", {{"count", std::to_string(*changed)},
                                         {"host", host_name()},
                                         {"time", iso_utc(std::chrono::system_clock::now())}});
    const git_commit* parents[] = {parent->first.get()};
    git_oid commit_id;
    TRY_GIT(git_commit_create(&commit_id, repo, "HEAD", signature->get(), signature->get(), nullptr,
                              message.c_str(), tree.get(), parent->first ? 1 : 0, parents));
    return changed;
}

std::expected<void, core::Error> fetch(git_remote* remote, const std::string& branch) {
    // '+' yalnız yerel takip dalını zorla günceller; uzak geçmişe yazılmaz.
    std::string refspec = "+" + local_ref(branch) + ":" + tracking_ref(branch);
    char* refspecs_raw[] = {refspec.data()};
    const git_strarray refspecs{refspecs_raw, 1};
    git_fetch_options options;
    TRY_GIT(git_fetch_options_init(&options, GIT_FETCH_OPTIONS_VERSION));
    options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    TRY_GIT(git_remote_fetch(remote, &refspecs, &options, nullptr));
    return {};
}

// Onların sürümü `<ad> (conflict YYYY-MM-DD HHMM).<uzantı>` olarak yanına yazılır; ad doluysa uzantıdan önce
// " 2", " 3" … eklenir. Dosya yalnız yoksa oluşturulur (O_EXCL): var olan hiçbir şeyin üzerine yazılmaz.
// Dönüş: depo-göreli, '/' ayırıcılı UTF-8 yol.
std::expected<std::string, core::Error> write_conflict_copy(git_repository* repo, const fs::path& workdir,
                                                            const std::string& path, const git_oid& blob_id,
                                                            const std::string& stamp) {
    git_blob* raw = nullptr;
    TRY_GIT(git_blob_lookup(&raw, repo, &blob_id));
    const Blob blob(raw);
    const std::string_view content(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                                   static_cast<std::size_t>(git_blob_rawsize(blob.get())));
    const fs::path original = path_from_utf8(path);
    const std::string stem = utf8(original.stem());
    const std::string extension = utf8(original.extension());  // uzantısız dosyada boş: nokta eklenmez
    for (int number = 1;; ++number) {
        const std::string suffix = number == 1 ? "" : " " + std::to_string(number);
        const fs::path name =
            original.parent_path() /
            path_from_utf8(std::format("{} (conflict {}){}{}", stem, stamp, suffix, extension));
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
// Her yol ve kopya diskten sahnelenir; sahneleme çakışma kaydını kapatır. Dönüş: oluşturulan kopyalar.
std::expected<std::vector<core::NotePath>, core::Error> resolve_conflicts(git_repository* repo,
                                                                          git_index* index) {
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

// Ayrışmış geçmiş: üç yollu merge → çakışma çözümü → iki ebeveynli commit. Ortak ata yoksa (ilgisiz geçmiş)
// hiçbir şeye dokunmadan hata: ilk kurulum enable'ın işi. Dönüş: değişen dosya sayısı.
std::expected<int, core::Error> merge_diverged(git_repository* repo, const std::string& branch,
                                               const git_annotated_commit* their,
                                               const core::SyncConfig& sync,
                                               std::vector<core::NotePath>& copies) {
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
    TRY_GIT(git_merge(repo, heads, 1, &merge_options, &checkout));

    // Bundan sonraki bir hata depoyu MERGE durumunda bırakır: sonraki senkron repo_busy_state ile durur,
    // iki tarafın içeriği HEAD'de ve uzak dalda durduğu için kayıp yoktur.
    git_index* index_raw = nullptr;
    TRY_GIT(git_repository_index(&index_raw, repo));
    const Index index(index_raw);
    auto resolved = resolve_conflicts(repo, index.get());
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
    const git_commit* parents[] = {ours->first.get(), their_commit.get()};
    git_oid commit_id;
    TRY_GIT(git_commit_create(&commit_id, repo, "HEAD", signature->get(), signature->get(), nullptr,
                              message.c_str(), tree.get(), 2, parents));
    TRY_GIT(git_repository_state_cleanup(repo));

    copies.insert(copies.end(), resolved->begin(), resolved->end());
    return count_changes(repo, ours->second.get(), tree.get());
}

// Döngü 4: güncel → 0; hızlı ileri (doğmamış yerel dal dahil) → checkout + dal ref'i; ayrışmış →
// merge_diverged. Dönüş: değişen dosya sayısı; çakışma kopyaları `copies`'e eklenir.
std::expected<int, core::Error> integrate(git_repository* repo, const std::string& branch,
                                          const core::SyncConfig& sync, std::vector<core::NotePath>& copies) {
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
    if (!(analysis & (GIT_MERGE_ANALYSIS_FASTFORWARD | GIT_MERGE_ANALYSIS_UNBORN)))
        return merge_diverged(repo, branch, their.get(), sync, copies);

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
        return changed;

    // SAFE: yerel değişikliğin üzerine yazmak yerine hata verir (ağaç az önce commit'lendi, olmamalı);
    // yok sayılan dosyalar da korunur. Önce checkout, sonra ref: checkout yarıda kalırsa dal eski commit'te
    // kalır, yarım güncelleme sonraki turda yerel değişiklik olarak commit'lenir — kayıp yok.
    git_checkout_options checkout;
    TRY_GIT(git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION));
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
    TRY_GIT(git_checkout_tree(repo, reinterpret_cast<const git_object*>(target.get()), &checkout));

    // Dal ref'i karşılaştır-ve-değiştir ile taşınır; HEAD dala sembolik bağlı olduğundan onunla gelir.
    git_reference* moved = nullptr;
    constexpr const char* kReflog = "noteapp sync: fast-forward";
    TRY_GIT(old_tip->first ? git_reference_create_matching(&moved, repo, local.c_str(), target_id, 1,
                                                           git_commit_id(old_tip->first.get()), kReflog)
                           : git_reference_create(&moved, repo, local.c_str(), target_id, 0, kReflog));
    git_reference_free(moved);
    return changed;
}

int record_rejection(const char* /*refname*/, const char* status, void* payload) {
    if (status != nullptr)
        *static_cast<std::string*>(payload) = status;
    return 0;
}

// Döngü 5: yerel dal uzaktakinden öndeyse push. Dönüş: push yapıldı mı. Ret "sync.push_rejected".
std::expected<bool, core::Error> push_if_ahead(git_repository* repo, git_remote* remote,
                                               const std::string& branch) {
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
    std::string rejection;  // sunucu ref'i güncellemeyi reddederse çağrı başarılı döner, ret buraya düşer
    options.callbacks.push_update_reference = record_rejection;
    options.callbacks.payload = &rejection;
    if (const int rc = git_remote_push(remote, &refspecs, &options); rc == GIT_ENONFASTFORWARD)
        rejection = git_error_last()->message;  // uzak dal fetch'ten sonra ilerlemiş
    else if (rc < 0)
        return std::unexpected(git_failure());
    if (!rejection.empty())
        return std::unexpected(
            sync_error(std::string(kPushRejected), {{"branch", branch}, {"detail", rejection}}));
    return true;
}

fs::path status_file() {
    return core::state_dir() / "sync-status.toml";
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

SyncService::SyncService(const core::Config& config) : config_(config) {}

std::expected<SyncResult, core::Error> SyncService::sync_now(const std::function<void()>& before_push) {
    const auto lock = SyncLock::acquire(core::state_dir() / "sync.lock",
                                        std::chrono::minutes(config_.tuning.sync_stale_lock_minutes));
    if (!lock) {
        // Durum dosyası kilidi tutan sürecindir; meşgulken yazılmaz.
        const auto log = lock.error().message_key == "sync.busy" ? core::log_info : core::log_error;
        log("sync: " + core::format_error(lock.error()));
        return std::unexpected(lock.error());
    }
    auto outcome = run_cycle(before_push);
    record(outcome);
    return outcome;
}

std::expected<SyncResult, core::Error> SyncService::run_cycle(const std::function<void()>& before_push) {
    const std::string notes_dir = utf8(config_.notes_dir);
    const core::Error not_initialized{
        core::Error::Code::NotFound, "sync.not_initialized", {{"path", notes_dir}}};
    git_repository* repo_raw = nullptr;
    if (const int rc = git_repository_open(&repo_raw, notes_dir.c_str()); rc == GIT_ENOTFOUND)
        return std::unexpected(not_initialized);
    else if (rc < 0)
        return std::unexpected(git_failure());
    const Repository repo(repo_raw);
    git_remote* remote_raw = nullptr;
    if (git_repository_is_bare(repo.get()) || git_remote_lookup(&remote_raw, repo.get(), kRemoteName) < 0)
        return std::unexpected(not_initialized);
    const Remote remote(remote_raw);

    // Elle başlatılmış ya da yarıda kalmış merge/rebase/cherry-pick: index'teki çakışmaları commit_all
    // işaretli dosyalarla "çözerdi". Kullanıcı bitirene (ya da iptal edene) kadar hiçbir şeye dokunulmaz.
    if (git_repository_state(repo.get()) != GIT_REPOSITORY_STATE_NONE)
        return std::unexpected(sync_error("sync.repo_busy_state", {{"path", notes_dir}}));

    const std::string& branch = config_.sync.branch;
    if (auto on_branch = check_head(repo.get(), branch); !on_branch)
        return std::unexpected(on_branch.error());

    SyncResult result;
    const auto committed = commit_all(repo.get(), branch, config_.sync);
    if (!committed)
        return std::unexpected(committed.error());
    result.committed = *committed;

    const auto pushed = retry_rejected_push([&]() -> std::expected<bool, core::Error> {
        if (auto fetched = fetch(remote.get(), branch); !fetched)
            return std::unexpected(fetched.error());
        const auto pulled = integrate(repo.get(), branch, config_.sync, result.conflict_copies);
        if (!pulled)
            return std::unexpected(pulled.error());
        result.pulled += *pulled;
        if (before_push)
            before_push();
        return push_if_ahead(repo.get(), remote.get(), branch);
    });
    if (!pushed)
        return std::unexpected(pushed.error());
    result.pushed = *pushed;
    return result;
}

void SyncService::record(const std::expected<SyncResult, core::Error>& outcome) const {
    SyncStatus status = this->status();  // önceki başarı zamanı hatada korunur
    if (outcome) {
        status.last_success = std::chrono::system_clock::now();
        status.last_error.reset();
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
    std::ostringstream text;
    text << table << '\n';
    // state_dir() kilit alınırken oluşturuldu.
    if (auto written = core::write_file_atomically(status_file(), text.str()); !written)
        core::log_error("sync: " + core::format_error(written.error()));
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
    return status;
}

}  // namespace noteapp::sync
