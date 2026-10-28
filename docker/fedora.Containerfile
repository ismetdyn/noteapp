# Fedora derleme/test imajı; bağımlılıklar imaj katmanında önbelleklenir (SPEC.md §3 Fedora paketleri).
FROM fedora:latest

RUN dnf install -y gcc-c++ cmake ninja-build git \
        qt6-qtbase-devel libsecret-devel openssl-devel pkgconf-pkg-config \
        dejavu-sans-fonts dejavu-sans-mono-fonts \
    && dnf clean all

WORKDIR /src
