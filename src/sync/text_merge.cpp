#include "sync/text_merge.hpp"

#include <algorithm>
#include <cstddef>
#include <git2.h>

namespace noteapp::sync {

// git_merge_file yazma kilidi altında koşar ve uygulama sınırı yoktur. Üstü çakışma kopyasına düşer. Temiz
// birleşimin çıktısı girdileri aşmaz, çerçeve sınırına (64 MiB) sığar: aynı birleşme her senkronda yeniden
// denenmez.
constexpr std::size_t kMaxMergeInputBytes = std::size_t{4} * 1024U * 1024U;

std::optional<std::string> merge_text(std::string_view base, std::string_view ours, std::string_view theirs) {
    if (std::max({base.size(), ours.size(), theirs.size()}) > kMaxMergeInputBytes)
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
    std::optional<std::string> merged;
    if (result.automergeable != 0)
        merged.emplace(result.ptr == nullptr ? "" : result.ptr, result.len);
    git_merge_file_result_free(&result);
    return merged;
}

}  // namespace noteapp::sync
