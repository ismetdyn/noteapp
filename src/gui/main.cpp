#include "core/config.hpp"
#include "core/dll_search.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"

#include <SDL3/SDL_main.h>
#include <cstdio>

int main(int /*argc*/, char* /*argv*/[]) {
    noteapp::core::restrict_dll_search_to_system32();
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer, pencere açılınca durum çubuğunda
    // görünür.
    const noteapp::core::LoadedConfig loaded = noteapp::core::load_config();
    noteapp::core::init_i18n(loaded.config.language);
    noteapp::core::init_log(loaded.config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        noteapp::core::log_warn(noteapp::core::format_error(warning));

    noteapp::gui::GuiState state(loaded.config);
    state.show_config_warnings(loaded.warnings);

    auto view = noteapp::gui::View::create(state);
    if (!view) {
        std::fprintf(stderr, "%s\n", view.error().c_str());
        return 1;
    }
    (*view)->install_deciders();
    (*view)->run();
    return 0;
}
