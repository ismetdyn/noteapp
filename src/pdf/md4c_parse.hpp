#pragma once

// md4c'yi okuyan iki yerin (pdf::parse_markdown — GUI önizlemesi ve PDF —, TUI markdown görünümü) ortak ayrıştırma
// ayarları ve saf yardımcıları. Çizim arayüze özgüdür, burada değil (SD5).

#include "pdf/markdown_blocks.hpp"

#include <cstddef>
#include <exception>
#include <md4c.h>
#include <string>
#include <string_view>

namespace noteapp::pdf {

// NUL (MD_TEXT_NULLCHAR) yerine yazılan karakter (U+FFFD).
inline constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

// Arayüzlerin ortak sözdizimi. MD_DIALECT_GITHUB'ın dipnot/uyarı blokları plan kapsamında değil; yalnız gereken
// bayraklar. MD_FLAG_NOHTML sözleşmedir: HTML düz metin kalır, not içeriği güvenilmez.
inline constexpr unsigned kMarkdownFlags =
    MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML;

// Özniteliğin ham metni (kod bloğunun dil etiketi gibi).
inline std::string_view raw_attribute(const MD_ATTRIBUTE& attribute) {
    return {attribute.text, attribute.size};
}

// Öznitelik metni; varlık parçaları çözülür (HTML'deki href gibi), NUL U+FFFD olur. Arındırma çağıranda. md4c wiki
// hedefini zaten varlıksız kurar.
std::string decoded_attribute(const MD_ATTRIBUTE& attribute);

Align align_of(MD_ALIGN align);

// `depth` iç içe liste derinliğidir (1 dış liste).
std::string_view list_bullet(std::size_t depth);

// Görev öğesinin imi ("[ ] ", "[x] "); görev değilse boş.
std::string_view task_marker(const MD_BLOCK_LI_DETAIL& item);

namespace md4c_internal {

template <class Handler>
struct Md4cCall {
    Handler& handler;
    std::exception_ptr error;
};

// md4c C'dir ve -fexceptions'sız derlenir (cmake/Dependencies.cmake): onun çerçevelerinden geçen exception
// tanımsızdır (unwind tablosu yoksa std::terminate, varsa md_parse'ın temizliği atlanır). Geri çağrı yakalar
// ve non-zero döner: md4c ayrıştırmayı durdurur, kendi belleğini bırakır; exception parse_md4c'de yeniden
// fırlar.
template <class Handler, class Event>
int md4c_guarded(void* userdata, Event event) noexcept {
    auto& call = *static_cast<Md4cCall<Handler>*>(userdata);
    try {
        event(call.handler);
        return 0;
    } catch (...) {
        call.error = std::current_exception();
        return 1;
    }
}

}  // namespace md4c_internal

// `handler` md4c olaylarını alır: enter_block(MD_BLOCKTYPE, const void*), leave_block(MD_BLOCKTYPE),
// enter_span(MD_SPANTYPE, const void*), leave_span(MD_SPANTYPE), text(MD_TEXTTYPE, std::string_view).
// İşleyicinin exception'ı ayrıştırmayı durdurur ve md4c döndükten sonra çağırana yayılır.
template <class Handler>
void parse_md4c(std::string_view text, unsigned flags, Handler& handler) {
    using Call = md4c_internal::Md4cCall<Handler>;
    MD_PARSER parser{};
    parser.flags = flags;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* userdata) {
        return md4c_internal::md4c_guarded<Handler>(userdata,
                                                    [&](Handler& on) { on.enter_block(type, detail); });
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* userdata) {
        return md4c_internal::md4c_guarded<Handler>(userdata, [&](Handler& on) { on.leave_block(type); });
    };
    parser.enter_span = [](MD_SPANTYPE type, void* detail, void* userdata) {
        return md4c_internal::md4c_guarded<Handler>(userdata,
                                                    [&](Handler& on) { on.enter_span(type, detail); });
    };
    parser.leave_span = [](MD_SPANTYPE type, void*, void* userdata) {
        return md4c_internal::md4c_guarded<Handler>(userdata, [&](Handler& on) { on.leave_span(type); });
    };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* userdata) {
        return md4c_internal::md4c_guarded<Handler>(
            userdata, [&](Handler& on) { on.text(type, std::string_view(value, size)); });
    };
    Call call{handler, nullptr};
    // md4c kendisi yalnız bellek yetmezse hata verir; o ana kadar gelenler kullanılır.
    md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &call);
    if (call.error)
        std::rethrow_exception(call.error);
}

}  // namespace noteapp::pdf
