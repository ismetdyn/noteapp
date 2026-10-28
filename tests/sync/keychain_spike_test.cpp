#include <doctest/doctest.h>
#include <keychain/keychain.h>
#include <string>

// Elle: gerçek OS kasasına yazar (Windows Credential Manager / libsecret / macOS Keychain); varsayılan koşuda
// kullanıcının kasasına dokunulmasın diye atlanır. Koşturma: sync_tests --no-skip -tc="*keychain*"
TEST_CASE("keychain round-trips a dummy secret (manual)" * doctest::skip()) {
    const std::string package = "noteapp";
    const std::string service = "noteapp-spike-test";
    const std::string user = "spike-user";
    const std::string secret = "dummy-secret-çğıİöşü";

    keychain::Error error;
    keychain::setPassword(package, service, user, secret, error);
    REQUIRE_MESSAGE(!error, error.message);

    // Okuma başarısız olsa da silme yapılsın diye CHECK: kasada artık sır kalmamalı.
    const std::string read_back = keychain::getPassword(package, service, user, error);
    CHECK_MESSAGE(!error, error.message);
    CHECK(read_back == secret);

    keychain::deletePassword(package, service, user, error);
    CHECK_MESSAGE(!error, error.message);

    keychain::getPassword(package, service, user, error);
    CHECK(error.type == keychain::ErrorType::NotFound);
}
