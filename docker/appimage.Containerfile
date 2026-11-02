# AppImage derleme imajı. glibc'si Fedora'dan eski taban: çıkan dosya yeni olmayan dağıtımlarda da açılır.
# Ubuntu 24.04: g++-14 ve CMake 3.28, projenin (C++23, CMakePresets sürüm 6) istediği en eski çift.
# Etiket: ubuntu:24.04. Özet çok mimarili imaj dizinine aittir; apt paketleri sabit değildir.
FROM docker.io/library/ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

# Paketler docker/fedora.Containerfile listesinin Ubuntu karşılığıdır (SDL3 X11/Wayland/D-Bus arka uçları dahil).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++-14 cmake ninja-build git patch curl ca-certificates pkg-config file \
        libsecret-1-dev libssl-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
        libdbus-1-dev libibus-1.0-dev libxkbcommon-dev libgl-dev libegl-dev libgles-dev \
        libwayland-dev wayland-protocols libdecor-0-dev \
    && rm -rf /var/lib/apt/lists

# Araçlar sürüm + SHA256 ile sabit; özetler GitHub release API'sinin `digest` alanıyla karşılaştırıldı.
# type2-runtime da sabit: --runtime-file verilmezse appimagetool `continuous` sürümünü indirir.
ARG APPIMAGETOOL_VERSION=1.9.1
ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG LINUXDEPLOY_VERSION=1-alpha-20251107-1
ARG LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ARG RUNTIME_VERSION=20251108
ARG RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
ARG RUNTIME_LICENSE_SHA256=aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1
RUN mkdir -p /tools \
    && curl -fsSL -o /tools/appimagetool \
        "https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage" \
    && curl -fsSL -o /tools/linuxdeploy \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage" \
    && curl -fsSL -o /tools/runtime-x86_64 \
        "https://github.com/AppImage/type2-runtime/releases/download/${RUNTIME_VERSION}/runtime-x86_64" \
    && curl -fsSL -o /tools/LICENSE-type2-runtime \
        "https://raw.githubusercontent.com/AppImage/type2-runtime/${RUNTIME_VERSION}/LICENSE" \
    && printf '%s  %s\n' \
        "$APPIMAGETOOL_SHA256" /tools/appimagetool \
        "$LINUXDEPLOY_SHA256" /tools/linuxdeploy \
        "$RUNTIME_SHA256" /tools/runtime-x86_64 \
        "$RUNTIME_LICENSE_SHA256" /tools/LICENSE-type2-runtime \
        | sha256sum -c - \
    && chmod +x /tools/appimagetool /tools/linuxdeploy

# Çalışma zamanına statik bağlananların lisansları. Kaynak: type2-runtime 20251108 src/runtime/Makefile ve
# scripts/common/install-dependencies.sh; sürümler packaging/linux/runtime-NOTICE.txt ile aynı tutulur.
RUN mkdir -p /tools/runtime-licenses \
    && cd /tools/runtime-licenses \
    && curl -fsSL -o libfuse-LICENSE https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LICENSE \
    && curl -fsSL -o libfuse-LGPL2.txt https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LGPL2.txt \
    && curl -fsSL -o libfuse-mount.c.diff \
        "https://raw.githubusercontent.com/AppImage/type2-runtime/${RUNTIME_VERSION}/patches/libfuse/mount.c.diff" \
    && curl -fsSL -o squashfuse-LICENSE https://raw.githubusercontent.com/vasi/squashfuse/0.5.2/LICENSE \
    && curl -fsSL -o zstd-LICENSE https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE \
    && curl -fsSL -o zlib-LICENSE https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE \
    && curl -fsSL -o mimalloc-LICENSE https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE \
    && curl -fsSL -o musl-COPYRIGHT "https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5" \
    && printf '%s  %s\n' \
        b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad libfuse-LICENSE \
        dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551 libfuse-LGPL2.txt \
        1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44 libfuse-mount.c.diff \
        9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13 squashfuse-LICENSE \
        7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8 zstd-LICENSE \
        845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243 zlib-LICENSE \
        19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d mimalloc-LICENSE \
        f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af musl-COPYRIGHT \
        | sha256sum -c -

# Konteynerde FUSE yok; araçlar kendini açıp çalışır.
ENV APPIMAGE_EXTRACT_AND_RUN=1
WORKDIR /src
