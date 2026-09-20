# Guacamole ESPKVM Protocol Plugin

Native Apache Guacamole protocol integration for ESPKVM devices.

This project allows ESPKVM devices to be added as normal Guacamole connections and accessed directly through the Guacamole interface.

## Project status

> **Early development / experimental**

The plugin is functional, but bugs and compatibility issues may still exist.

Bug reports, testing feedback, fixes, and pull requests are welcome.

## Current features

- Native `espkvm` protocol in Apache Guacamole
- ESPKVM authentication
- Native ESPKVM web interface inside Guacamole
- Keyboard and mouse / USB OTG control
- ESPKVM REST API bridge
- H.264 video transport
- MJPEG stream transport
- Multiple ESPKVM devices as separate Guacamole connections
- ESPKVM credentials remain on the Guacamole server side

## Tested environment

Currently tested with:

- Apache Guacamole 1.6.0
- guacd 1.6.0
- Debian Linux
- x86_64 / amd64
- ESPKVM on ESP32-P4

Other environments may work but have not yet been fully tested.

## Installation

Clone the repository and run the installer:

```bash
git clone https://github.com/Crisspii/guacamole-espkvm.git
cd guacamole-espkvm
sudo ./install.sh
```

To uninstall:

```bash
sudo ./uninstall.sh
```

## Bugs and contributions

This project is still in an early stage.

If you find a bug, please open a GitHub Issue.

Testing feedback, fixes, and pull requests are welcome.

Please do not include passwords, session cookies, or other credentials in bug reports.

## License

Licensed under the Apache License 2.0. See LICENSE.
