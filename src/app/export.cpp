#include "app/export.hpp"

#include "core/note_store.hpp"
#include "pdf/pdf_export.hpp"

#include <system_error>

namespace noteapp::app {

std::string default_export_name(const core::NotePath& note) {
    return core::split_note_path(note.string()).name + ".pdf";
}

bool export_target_exists(const std::filesystem::path& output) {
    std::error_code ignored;
    return std::filesystem::exists(std::filesystem::symlink_status(output, ignored));
}

std::expected<void, core::Error> export_note_pdf(std::string_view bytes, const core::NotePath& note,
                                                 const std::filesystem::path& output) {
    if (core::looks_binary(bytes))
        return std::unexpected(core::Error{
            core::Error::Code::InvalidPath, std::string(kExportBinaryKey), {{"path", note.string()}}});
    return pdf::export_pdf(bytes, pdf::format_for(note.value()), output);
}

}  // namespace noteapp::app
