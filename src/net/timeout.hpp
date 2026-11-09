#pragma once

#include <chrono>

// http_get'in iç ayrıntısı; ayrı başlıkta durması yalnız eşlemenin birim testi içindir (tests/net).
namespace noteapp::net::detail {

// HttpRequest zaman aşımlarında 0 = sınırsız (core::Limit anlamı). curl'de ikisi farklıdır: CURLOPT_TIMEOUT=0
// sınırsızdır, CURLOPT_CONNECTTIMEOUT=0 ise 300 sn'lik yerleşik varsayılandır ve bağlantı için "sınırsız" değeri
// yoktur. Bu yüzden 0'da long'un en büyüğü verilir; curl onu kendi zaman türünün üst sınırına kırpar
// (lib/setopt.c setopt_set_timeout_sec), pratikte sınırsızdır. long'a sığmayan süre de long'un en büyüğüne
// kırpılır (Windows'ta long 32 bittir). Negatif süre çağıranın hatasıdır: std::invalid_argument.
long curl_connect_timeout(std::chrono::seconds timeout);
long curl_total_timeout(std::chrono::seconds timeout);

}  // namespace noteapp::net::detail
