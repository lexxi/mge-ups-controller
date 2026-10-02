# MGE UPS Controller

ESP8266 / LOLIN D1 mini based controller for an MGE UPS.

## Current status

### v0.1 – WLAN configuration

- ESP8266 web server
- WLAN client mode
- fallback access point when no configured WLAN is available
- WLAN configuration via web interface
- configuration stored in LittleFS
- no UPS protocol implementation yet

## Hardware

- LOLIN/WEMOS D1 mini (ESP8266)
- UPS connection via ADuM1201 planned
- UPS serial interface: 2400 8N1
- planned software serial pins: RX D5, TX D6

## Development

The Arduino sketch is in src/mge-ups-controller.ino.

## Roadmap

1. WLAN configuration and web UI
2. UPS raw serial monitoring
3. MGE protocol parsing
4. UPS status and measurements
5. Configuration and monitoring improvements
