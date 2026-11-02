# Windows çapraz derleme + Wine test imajı (scripts/build-windows.sh). MinGW kütüphanesi kurulmaz: bağımlılıklar
# FetchContent'ten statik gelir. libstdc++.a mingw64-gcc-c++ içindedir; winpthreads'in statik hali ayrı pakettedir.
# poppler-utils: Fedora'da pdftotext.exe paketi yok; Wine'daki PDF testleri host pdftotext'ini çağırır (tests/pdf).
# Etiket: fedora:44. Özet çok mimarili imaj dizinine aittir; dnf paketleri sabit değildir.
FROM registry.fedoraproject.org/fedora@sha256:b414c09be79dd1f32529b55b096b6bb30435d82ee16fbdbbabf8985eb5f86db0

RUN dnf install -y --setopt=install_weak_deps=False \
        cmake ninja-build tar patch poppler-utils \
        mingw64-gcc-c++ mingw64-winpthreads-static \
        wine-core.x86_64 wine-common \
    && dnf clean all

# Wine dosya adlarını yerel ayarın kod sayfasıyla çevirir; varsayılan POSIX yerelinde Türkçe yollar açılamaz.
ENV LC_ALL=C.UTF-8

WORKDIR /src
