#!/bin/sh
# Downloads what the build needs into third_party/: Flite, the speech engine
# from Carnegie Mellon University (a fixed version), and mbedTLS 3.6 LTS (the
# secure connection). Run it once before make. mbedTLS comes from its release
# tarball, not a git clone: it includes generated files.
set -e
COMMIT=6c9f20dc915b17f5619340069889db0aa007fcdc
VER=3.6.2
cd "$(dirname "$0")"
mkdir -p third_party
# GitHub's downloads sometimes answer with a passing error (504 and the like):
# each one is tried up to 6 times, a little apart, before giving up.
fetch() {
    echo "Downloading $1"
    curl -fL --retry 5 --retry-delay 15 --retry-all-errors --connect-timeout 30 -o "$2" "$1"
}
if [ ! -f third_party/flite/src/synth/flite.c ]; then
    URL="https://github.com/festvox/flite/archive/$COMMIT.tar.gz"
    fetch "$URL" third_party/flite.tar.gz
    tar -xzf third_party/flite.tar.gz -C third_party
    mv "third_party/flite-$COMMIT" third_party/flite
    rm third_party/flite.tar.gz
fi
if [ ! -f third_party/mbedtls/library/ssl_tls.c ]; then
    URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2"
    fetch "$URL" third_party/mbedtls.tar.bz2
    tar -xjf third_party/mbedtls.tar.bz2 -C third_party
    mv "third_party/mbedtls-$VER" third_party/mbedtls
    rm third_party/mbedtls.tar.bz2
fi
echo "Done. Now run: make"
