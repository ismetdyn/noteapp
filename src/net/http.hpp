// src/net/http.hpp
#pragma once
#include "core/error.hpp"
#include "core/limits.hpp"
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
namespace noteapp::net {
struct HttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    core::Limit max_bytes;
    std::chrono::seconds connect_timeout{30};
    std::chrono::seconds total_timeout{900};
};
struct HttpResponse {
    long status = 0;
    std::string body;
    std::optional<long> ratelimit_remaining;
    std::optional<std::int64_t> ratelimit_reset;  // unix saniye
    std::optional<std::int64_t> retry_after;      // saniye
};
std::expected<HttpResponse, core::Error> http_get(const HttpRequest& request, std::stop_token stop);
}  // namespace noteapp::net
