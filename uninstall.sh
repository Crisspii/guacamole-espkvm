#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run this script as root or with sudo."
    exit 1
fi

echo "Removing Guacamole ESPKVM plugin..."

rm -f /usr/local/lib/libguac-client-espkvm.so
rm -f /usr/local/lib/libguac-client-espkvm.so.0
rm -f /usr/local/lib/libguac-client-espkvm.so.0.0.0
rm -f /usr/local/lib/libguac-client-espkvm.la
rm -f /usr/local/lib/libguac-client-espkvm.a

rm -f /etc/guacamole/protocols/espkvm.json

rm -f /etc/guacamole/extensions/guacamole-espkvm-ui.jar
rm -f /etc/guacamole/extensions/guacamole-espkvm-ui-0.1.0.jar

ldconfig

systemctl restart guacd 2>/dev/null || true

for SERVICE in tomcat tomcat9 tomcat10; do
    if systemctl list-unit-files "${SERVICE}.service" 2>/dev/null \
        | grep -q "${SERVICE}.service"; then
        systemctl restart "$SERVICE" || true
        break
    fi
done

echo
echo "ESPKVM plugin removed."
