# Çalışan konteyner motorunu `engine` değişkenine yazar: `source scripts/container-engine.sh`
# `docker` kurulu olsa da servis kapalı veya yetki yok olabilir; kurulu olması yetmez, çalışıyor olmalı.
if docker info >/dev/null 2>&1; then
    engine=docker
elif command -v podman >/dev/null 2>&1; then
    engine=podman
else
    echo "ERROR: docker or podman is required" >&2
    exit 1
fi
