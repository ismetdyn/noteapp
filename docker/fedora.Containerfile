# Fedora derleme/test imajı; bağımlılıklar imaj katmanında önbelleklenir (SPEC.md §3 Fedora paketleri).
FROM fedora:latest

# Son üç satır SDL3'ün X11/Wayland/D-Bus arka uçları içindir; poppler-utils PDF testlerinin pdftotext'i.
RUN dnf install -y gcc-c++ cmake ninja-build git patch \
        libsecret-devel openssl-devel pkgconf-pkg-config poppler-utils \
        libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel \
        libXtst-devel dbus-devel ibus-devel libxkbcommon-devel mesa-libGL-devel mesa-libEGL-devel mesa-libGLES-devel \
        wayland-devel wayland-protocols-devel libdecor-devel \
    && dnf clean all

# scripts/check-sanitize.sh için ayrı katman: üstteki paket katmanını geçersiz kılmaz.
RUN dnf install -y libasan libubsan && dnf clean all

# tests/net: yerel HTTPS sunucusu (Python http.server) ve her koşuda üretilen test CA'sı (openssl CLI).
RUN dnf install -y python3 openssl && dnf clean all

WORKDIR /src
