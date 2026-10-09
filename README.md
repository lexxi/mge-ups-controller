# MGE UPS Controller

ESP8266 / LOLIN D1 mini based controller for legacy MGE Ellipse UPS devices using the MGE SHUT (Serial HID UPS Transfer) protocol.

The project started as a serial reverse-engineering effort and now provides stable monitoring, JSON APIs and verified remote control for shutdown and startup timers.

## Current status

### v0.12.0

Implemented and verified:

- ESP8266 web server
- WLAN client mode
- fallback access point
- WLAN configuration stored in LittleFS
- persistent system logging
- MGE SHUT communication at 2400 8N1
- HID feature report polling
- spontaneous SHUT notification handling
- UPS status detection
- mains / battery operation detection
- battery capacity
- runtime to empty
- output voltage
- load percentage
- battery history in RAM
- complete HID report descriptor readout
- JSON status and health API
- engineering GET_REPORT / SET_REPORT API
- verified shutdown countdown
- verified startup countdown
- control API using seconds
- web UI control buttons and live timer display

## Hardware

- LOLIN / WEMOS D1 mini (ESP8266)
- UPS serial interface: 2400 baud, 8N1
- SoftwareSerial:
  - RX: D6
  - TX: D5
- isolated serial interface recommended for permanent installation

See [docs/hardware.md](docs/hardware.md) for hardware details.

## Protocol

The UPS uses **MGE SHUT – Serial HID UPS Transfer**.

The SHUT transport carries USB HID-style reports over the serial link.

Important verified HID reports:

| Report | Meaning | Notes |
| --- | --- | --- |
| `0x02` | PresentStatus | UPS, mains, charging and fault state |
| `0x0E` | Output voltage + load | actively polled |
| `0x0F` | DelayBeforeShutdown | 24-bit signed, 1 second per raw unit |
| `0x11` | DelayBeforeStartup | 24-bit signed, 10 seconds per raw unit |
| `0x16` | RemainingCapacity + RunTimeToEmpty | actively polled |

The tested Ellipse reports a 532-byte HID report descriptor.

## Web interface

The built-in web UI provides:

- UPS status
- mains / battery state
- decoded PresentStatus flags
- battery percentage
- runtime
- output voltage
- load percentage
- battery history
- SHUT communication counters
- raw RX view
- shutdown/startup timer status
- control buttons for shutdown/startup and cancel operations
- system log viewer

## API

The controller exposes JSON APIs for monitoring, control and engineering/debug use.

Main endpoints:

```text
GET  /api/status
GET  /api/health
GET  /api/control/status

POST /api/control/shutdown?delay=30
POST /api/control/startup?delay=300
POST /api/control/shutdown/cancel
POST /api/control/startup/cancel
```

Engineering endpoints:

```text
GET  /api/hid/get?id=0E&len=3
POST /api/hid/set?id=0F&data=0F%20FF%20FF%20FF&confirm=YES
GET  /api/hid/report-descriptor
```

Full API documentation is available in [docs/api.md](docs/api.md).

## Verified control behaviour

Shutdown and startup control were tested directly against the UPS.

### Shutdown

HID Report `0x0F` starts a real countdown. When it reaches zero, the UPS output switches off.

Example:

```text
POST /api/control/shutdown?delay=30
```

### Startup

HID Report `0x11` starts the startup countdown.

The HID descriptor defines a unit exponent of `+1`, so one raw unit represents 10 seconds. The public API hides this detail and accepts seconds.

Example:

```text
POST /api/control/startup?delay=300
```

Both timers use `-1` internally for "inactive / cancelled".

## Project structure

```text
.
├── docs/
│   ├── api.md
│   └── hardware.md
├── engineering/
│   ├── README.md
│   └── upstream/
│       └── nut/
├── src/
│   └── mge-ups-controller/
│       └── mge-ups-controller.ino
└── tools/
```

The `engineering/` directory contains protocol notes and pinned upstream Network UPS Tools reference sources used during the SHUT/HID reverse-engineering work.

## Development

The Arduino sketch is located at:

```text
src/mge-ups-controller/mge-ups-controller.ino
```

Current firmware version: **0.12.0**

## Security

The current firmware does not implement authentication for the HTTP control endpoints.

Use the controller only on a trusted network or restrict access externally.

## Upstream references

Protocol implementation work was cross-checked against historical Network UPS Tools (NUT) MGE SHUT sources and MGE HID documentation.

See [engineering/README.md](engineering/README.md) for pinned upstream references and reverse-engineering notes.


## WLAN / Roaming

Firmware v0.12.3 ergänzt die WLAN-Diagnose um Scan und RSSI-basiertes Roaming:

- Scan der sichtbaren 2,4-GHz-WLANs auf `/config`
- Anzeige von SSID, BSSID, RSSI, Kanal und Verschlüsselung
- Roaming-Prüfung alle 60 Sekunden, wenn das aktuelle Signal unter dem konfigurierten Schwellwert liegt
- Standard-Schwellwert: `-72 dBm`
- AP-Wechsel nur bei mindestens `4 dB` Verbesserung
- Roaming-Versuche und erfolgreiche Roams werden auf der WLAN-Seite gezählt
- Reconnect/Fallback-AP-Logik bleibt aktiv

Die Roaming-Werte werden zusammen mit der WLAN-Konfiguration in `/wifi.cfg` gespeichert.
