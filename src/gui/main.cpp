#include "app/ui_args.hpp"
#include "core/config.hpp"
#include "core/console.hpp"
#include "core/dll_search.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/gui_state.hpp"
#include "gui/view.hpp"

#include <SDL3/SDL_main.h>
#include <cstdio>

namespace {

constexpr int kExitUsage = 2;

}  // namespace

int main(int argc, char* argv[]) {
    noteapp::core::restrict_dll_search_to_system32();
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer, pencere açılınca durum çubuğunda
    // görünür.
    const noteapp::core::LoadedConfig loaded = noteapp::core::load_config();
    noteapp::core::init_i18n(loaded.config.language);
    noteapp::core::init_log(loaded.config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        noteapp::core::log_warn(noteapp::core::format_error(warning));

    // Pencere açılmadan: içe alma GUI'de diyalog ve sürükle-bıraktır, komut satırında alt komut yok.
    const auto args = noteapp::app::parse_ui_args(noteapp::core::utf8_arguments(argc, argv), false);
    if (args.kind == noteapp::app::UiArgs::Kind::Help) {
        std::fputs(noteapp::core::tr("gui.usage").c_str(), stdout);
        return 0;
    }
    if (args.kind == noteapp::app::UiArgs::Kind::Invalid) {
        if (!args.error_key.empty())
            std::fprintf(stderr, "%s\n", noteapp::core::tr(args.error_key, args.error_args).c_str());
        std::fputs(noteapp::core::tr("gui.usage").c_str(), stderr);
        return kExitUsage;
    }

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
