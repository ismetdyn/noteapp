#include "sync/text_merge.hpp"

#include "sync/git_free.hpp"

#include <algorithm>
#include <cstddef>
#include <git2.h>
#include <memory>

namespace noteapp::sync {

// git_merge_file yazma kilidi altında koşar ve kendi sınırı yoktur; süresi girdi boyu ve fark sayısıyla
// büyür. Sınır config'tedir (limits.sync.merge_input_bytes): üstü çakışma kopyasına düşer, içerik kaybolmaz.
// Temiz birleşimin çıktısı yazılır, aynı birleşme her senkronda yeniden denenmez.
std::optional<std::string> merge_text(std::string_view base, std::string_view ours, std::string_view theirs,
                                      std::size_t max_input_bytes) {
    if (std::max({base.size(), ours.size(), theirs.size()}) > max_input_bytes)
        return std::nullopt;
    git_merge_file_input inputs[3];
    const std::string_view texts[3] = {base, ours, theirs};
    for (int i = 0; i < 3; ++i) {
        if (git_merge_file_input_init(&inputs[i], GIT_MERGE_FILE_INPUT_VERSION) < 0)
            return std::nullopt;
        inputs[i].ptr = texts[i].empty() ? "" : texts[i].data();
        inputs[i].size = texts[i].size();
    }
    git_merge_file_options options;
    if (git_merge_file_options_init(&options, GIT_MERGE_FILE_OPTIONS_VERSION) < 0)
        return std::nullopt;
    git_merge_file_result result{};
    if (git_merge_file(&result, &inputs[0], &inputs[1], &inputs[2], &options) < 0)
        return std::nullopt;
    const std::unique_ptr<git_merge_file_result, GitFree<git_merge_file_result_free>> owner(&result);
    if (result.automergeable == 0)
        return std::nullopt;
    return std::string(result.ptr == nullptr ? "" : result.ptr, result.len);
}

}  // namespace noteapp::sync
