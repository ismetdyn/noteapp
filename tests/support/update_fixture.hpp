#pragma once

// Güncelleme işçisinin ve `note update` akışının testleri için sahte GitHub: son release'in JSON'u,
// SHA256SUMS ve paket bellekte. Testler ağa çıkmaz.
#include "app/update_install.hpp"
#include "app/updater.hpp"
#include "core/error.hpp"
#include "net/http.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::test {

inline constexpr std::string_view kLatestApiUrl =
    "https://api.github.com/repos/ismetdyn/noteapp/releases/latest";
inline constexpr std::string_view kReleaseDownloadPrefix =
    "https://github.com/ismetdyn/noteapp/releases/download/";
// FIPS 180-2 örneği: SHA-256("abc").
inline constexpr std::string_view kUpdatePayload = "abc";
inline constexpr std::string_view kUpdatePayloadSha =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

inline std::string release_download_url(std::string_view tag, std::string_view name) {
    return std::string(kReleaseDownloadPrefix) + std::string(tag) + "/" + std::string(name);
}

inline net::HttpResponse http_reply(long status, std::string body = {}) {
    net::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

// v<version> release'i: iki platformun paketi ve SHA256SUMS, adresleri aynı release'te.
inline std::string latest_release_json(std::string_view version) {
    const std::string v(version);
    const std::string tag = "v" + v;
    std::string json = R"({"tag_name":")" + tag + R"(","assets":[)";
    const std::vector<std::string> names = {"noteapp-" + v + "-x86_64.AppImage",
                                            "noteapp-" + v + "-windows-x86_64.zip", "SHA256SUMS"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        json += (i == 0 ? "" : ",");
        json += R"({"name":")" + names[i] + R"(","size":3,"browser_download_url":")" +
                release_download_url(tag, names[i]) + R"("})";
    }
    return json + "]}";
}

// URL -> cevap; bilinmeyen URL 404. `hold` açıkken istek `release()` ya da iptal gelene dek bekler
// (iptalde "net.cancelled" döner). İstekler sayılır.
class FakeGitHub {
public:
    // Son release `version`; paket kUpdatePayload, SHA256SUMS iki paketin doğru satırı.
    void publish(std::string_view version) {
        const std::lock_guard lock(mutex_);
        const std::string v(version);
        const std::string tag = "v" + v;
        responses_[std::string(kLatestApiUrl)] = http_reply(200, latest_release_json(version));
        const std::string appimage = "noteapp-" + v + "-x86_64.AppImage";
        const std::string zip = "noteapp-" + v + "-windows-x86_64.zip";
        responses_[release_download_url(tag, "SHA256SUMS")] =
            http_reply(200, std::string(kUpdatePayloadSha) + "  " + appimage + "\n" +
                                std::string(kUpdatePayloadSha) + "  " + zip + "\n");
        responses_[release_download_url(tag, appimage)] = http_reply(200, std::string(kUpdatePayload));
        responses_[release_download_url(tag, zip)] = http_reply(200, std::string(kUpdatePayload));
    }
    void respond(std::string url, std::expected<net::HttpResponse, core::Error> response) {
        const std::lock_guard lock(mutex_);
        responses_[std::move(url)] = std::move(response);
    }
    void hold() {
        const std::lock_guard lock(mutex_);
        held_ = true;
        held_url_.clear();
    }
    // Yalnız bu adrese giden istek bekler.
    void hold_only(std::string url) {
        const std::lock_guard lock(mutex_);
        held_ = true;
        held_url_ = std::move(url);
    }
    void release() {
        {
            const std::lock_guard lock(mutex_);
            held_ = false;
        }
        changed_.notify_all();
    }
    // `count` istek beklemeye girdi mi (hold açıkken); en çok 10 s bekler.
    bool wait_for_waiting(std::size_t count = 1) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10), [&] { return waiting_ >= count; });
    }
    std::size_t waiting() const {
        const std::lock_guard lock(mutex_);
        return waiting_;
    }
    std::size_t requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

    app::HttpGet get() {
        return [this](const net::HttpRequest& request,
                      std::stop_token stop) -> std::expected<net::HttpResponse, core::Error> {
            std::unique_lock lock(mutex_);
            ++requests_;
            if (held_ && (held_url_.empty() || held_url_ == request.url)) {
                ++waiting_;
                changed_.notify_all();
                changed_.wait(lock, stop, [&] { return !held_; });
                if (stop.stop_requested())
                    return std::unexpected(core::Error{core::Error::Code::Io, "net.cancelled", {}});
            }
            const auto found = responses_.find(request.url);
            if (found == responses_.end())
                return http_reply(404);
            return found->second;
        };
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<std::string, std::expected<net::HttpResponse, core::Error>> responses_;
    bool held_ = false;
    std::string held_url_;  // boş: her istek
    std::size_t waiting_ = 0;
    std::size_t requests_ = 0;
};

// Kurulumu taklit eder: aldığı paketi saklar, verilen sonucu döner.
struct FakeInstaller {
    app::InstallResult result = app::InstallOutcome{};
    std::mutex mutex;
    std::vector<std::string> installed;  // her çağrının paketi

    app::UpdateInstaller installer() {
        return [this](const app::InstallTarget&, const app::ReleaseInfo&, std::string_view bytes,
                      const core::UpdateLimits&, std::stop_token) {
            const std::lock_guard lock(mutex);
            installed.emplace_back(bytes);
            return result;
        };
    }
    std::size_t calls() {
        const std::lock_guard lock(mutex);
        return installed.size();
    }
};

inline app::UpdateSettings update_settings(std::string current = "1.2.3") {
    app::UpdateSettings s;
    s.limits = {.api_bytes = {1024 * 1024, "1MiB"},
                .sums_bytes = {64 * 1024, "64KiB"},
                .download_bytes = {512 * 1024 * 1024, "512MiB"},
                .connect_timeout = {30000, "30s"},
                .transfer_timeout = {900000, "15min"},
                .install_timeout = {600000, "10min"},
                .tool_output_bytes = {1024 * 1024, "1MiB"}};
    s.current_version = std::move(current);
    return s;
}

// Kendini güncelleyebilen bir kurulum (yol sahte; kurulumu FakeInstaller yapar).
inline app::InstallTarget installable_target() {
    return app::InstallTarget{.kind = app::InstallKind::LocalInstall, .path = "/fake/noteapp/1.2.3"};
}

}  // namespace noteapp::test
