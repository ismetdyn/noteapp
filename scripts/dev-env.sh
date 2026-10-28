# Git Bash'te Windows derleme ortamı: `source scripts/dev-env.sh`
# Varsayılanlar Qt Online Installer düzenine göredir; makineye göre ortam değişkenleriyle ezilebilir.
QT_ROOT="${QT_ROOT:-/c/Qt}"
QT_VERSION="${QT_VERSION:-6.12.0}"
QT_MINGW="${QT_MINGW:-mingw1310_64}"

export NOTEAPP_QT_DIR="$(cygpath -m "$QT_ROOT/$QT_VERSION/mingw_64")"
export PATH="$QT_ROOT/Tools/$QT_MINGW/bin:$QT_ROOT/Tools/Ninja:$QT_ROOT/Tools/CMake_64/bin:$QT_ROOT/Tools/QtCreator/bin/clang/bin:$PATH"
