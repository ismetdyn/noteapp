#pragma once

#include "app/sync_indicator.hpp"

#include <string>

namespace noteapp::tui {

// Üst çubuktaki senkron parçası (SPEC-tui): model app::SyncIndicator (GUI ile ortak); burada 60 sütuna sığan
// metni.
using SyncIndicator = app::SyncIndicator;

// kapalı / ⟳ / ✓ saat / ⚠ kısa neden. Tam hata metni durum satırındadır (SyncIndicator::error_text).
std::string sync_text(const SyncIndicator& indicator);

}  // namespace noteapp::tui
