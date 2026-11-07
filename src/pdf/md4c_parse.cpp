#include "pdf/md4c_parse.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <iterator>

namespace noteapp::pdf {

std::string decoded_attribute(const MD_ATTRIBUTE& attribute) {
    const std::string_view raw = raw_attribute(attribute);
    std::string out;
    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET begin = attribute.substr_offsets[i];
        const std::string_view part = raw.substr(begin, attribute.substr_offsets[i + 1] - begin);
        if (attribute.substr_types[i] == MD_TEXT_ENTITY)
            out += core::decode_html_entity(part);
        else if (attribute.substr_types[i] == MD_TEXT_NULLCHAR)
            out += kReplacementCharacter;
        else
            out += part;
    }
    return out;
}

Align align_of(MD_ALIGN align) {
    switch (align) {
    case MD_ALIGN_LEFT:
        return Align::Left;
    case MD_ALIGN_CENTER:
        return Align::Center;
    case MD_ALIGN_RIGHT:
        return Align::Right;
    default:
        return Align::Default;
    }
}

std::string_view list_bullet(std::size_t depth) {
    return kListBullets[(std::max<std::size_t>(depth, 1) - 1) % std::size(kListBullets)];
}

std::string_view task_marker(const MD_BLOCK_LI_DETAIL& item) {
    if (!item.is_task)
        return {};
    return item.task_mark == ' ' ? "[ ] " : "[x] ";
}

}  // namespace noteapp::pdf
