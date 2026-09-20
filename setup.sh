#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run this installer as root or with sudo."
    exit 1
fi

echo "========================================"
echo " Guacamole ESPKVM Plugin Installer"
echo "========================================"
echo

# Check Guacamole
if ! command -v guacd >/dev/null 2>&1 && [ ! -x /usr/local/sbin/guacd ]; then
    echo "ERROR: guacd was not found."
    echo "Please install Apache Guacamole first."
    exit 1
fi

# Check Guacamole headers
if [ ! -f /usr/local/include/guacamole/client.h ] && \
   [ ! -f /usr/include/guacamole/client.h ]; then
    echo "ERROR: Guacamole development headers were not found."
    exit 1
fi

echo "[1/5] Installing build dependencies..."

if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkg-config \
        libcurl4-openssl-dev \
        libjson-c-dev \
        default-jdk-headless
else
    echo "ERROR: Automatic installation currently supports Debian/Ubuntu only."
    exit 1
fi

echo "[2/5] Building native ESPKVM protocol..."

cd "$PROJECT_DIR"

autoreconf -fi
./configure
make -j"$(nproc)"

echo "[3/5] Installing native protocol..."

make install
ldconfig

echo "[4/5] Installing Guacamole protocol definition and UI extension..."

mkdir -p /etc/guacamole/protocols
mkdir -p /etc/guacamole/extensions

install -m 0644 \
    "$PROJECT_DIR/protocol/espkvm.json" \
    /etc/guacamole/protocols/espkvm.json

cd "$PROJECT_DIR/ui-extension"

rm -f /tmp/guacamole-espkvm-ui-0.1.0.jar

jar cf /tmp/guacamole-espkvm-ui-0.1.0.jar \
    guac-manifest.json \
    js \
    translations

install -m 0644 \
    /tmp/guacamole-espkvm-ui-0.1.0.jar \
    /etc/guacamole/extensions/guacamole-espkvm-ui-0.1.0.jar

echo "[5/5] Restarting Guacamole services..."

systemctl restart guacd 2>/dev/null || true

for SERVICE in tomcat tomcat9 tomcat10; do
    if systemctl list-unit-files "${SERVICE}.service" 2>/dev/null \
        | grep -q "${SERVICE}.service"; then
        systemctl restart "$SERVICE"
        break
    fi
done

echo
echo "========================================"
echo " ESPKVM plugin installed successfully"
echo "========================================"
echo
echo "Open Apache Guacamole and create a new connection."
echo "Protocol: ESPKVM"
echo
