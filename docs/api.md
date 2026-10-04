# API documentation

Firmware: v0.12.0

The controller exposes a small HTTP/JSON API for monitoring, control and protocol engineering.

## Status API

### GET /api/status

Returns UPS state, decoded PresentStatus flags, battery telemetry, voltage/load and communication/system information.

Example:

```json
{
  "version": "0.12.0",
  "ups": {
    "status": "EIN",
    "power": "NETZ",
    "operation": "ONLINE",
    "ac_present": true,
    "charging": false,
    "discharging": false,
    "good": true
  },
  "telemetry": {
    "valid": true,
    "battery_percent": 86,
    "runtime_seconds": 1475,
    "input_voltage": 230,
    "load_percent": 2
  }
}
```

### GET /api/health

Returns HTTP 200 when SHUT communication/telemetry is available, otherwise HTTP 503.

## Control API

These endpoints use the verified MGE SHUT/HID feature reports.

### GET /api/control/status

Returns both timer values.

```json
{
  "ok": true,
  "shutdown": {
    "raw": -1,
    "seconds": null
  },
  "startup": {
    "raw": -1,
    "seconds": null
  }
}
```

A raw value of `-1` means that no timer is active.

### POST /api/control/shutdown?delay=<seconds>

Starts the shutdown countdown.

Internally this writes HID Feature Report `0x0F` (DelayBeforeShutdown).

Example:

```powershell
Invoke-WebRequest -Method POST `
  -Uri "http://192.168.98.37/api/control/shutdown?delay=30"
```

The shutdown timer uses one raw unit per second.

### POST /api/control/shutdown/cancel

Cancels an active shutdown countdown by writing `-1` (`FF FF FF`) to Report `0x0F`.

### POST /api/control/startup?delay=<seconds>

Starts the startup countdown.

Internally this writes HID Feature Report `0x11` (DelayBeforeStartup).

Example:

```powershell
Invoke-WebRequest -Method POST `
  -Uri "http://192.168.98.37/api/control/startup?delay=300"
```

Report `0x11` has HID Unit Exponent `+1`. One raw unit therefore represents 10 seconds. The API hides this scaling and accepts seconds.

Requested seconds are rounded up to the next 10-second unit.

### POST /api/control/startup/cancel

Cancels an active startup countdown by writing `-1` (`FF FF FF`) to Report `0x11`.

## Engineering API

The engineering endpoints are intended for SHUT/HID reverse engineering and diagnostics.

### GET /api/hid/get?id=<hex>&len=<1..8>

Reads a HID Feature Report.

Examples:

```text
/api/hid/get?id=02&len=3
/api/hid/get?id=0E&len=3
/api/hid/get?id=0F&len=4
/api/hid/get?id=11&len=4
/api/hid/get?id=16&len=4
```

### POST /api/hid/set?id=<hex>&data=<hex>&confirm=YES

Writes a HID Feature Report.

The explicit `confirm=YES` parameter is required.

Example of writing the inactive value back to shutdown Report `0x0F`:

```powershell
Invoke-WebRequest -Method POST `
  -Uri "http://192.168.98.37/api/hid/set?id=0F&data=0F%20FF%20FF%20FF&confirm=YES"
```

This endpoint is intentionally limited to SHUT/HID SET_REPORT operations. It is not a generic raw-serial transmit endpoint.

### GET /api/hid/report-descriptor

Reads HID descriptor `0x21`, obtains the advertised report-descriptor length and then retrieves the complete HID report descriptor `0x22`.

For the tested MGE Ellipse:

- HID descriptor length: 9 bytes
- Report descriptor length: 532 bytes

## Verified reports

| Report | Meaning | Notes |
| --- | --- | --- |
| 0x02 | PresentStatus | UPS/network/charging state |
| 0x0E | Output voltage + load | actively polled |
| 0x0F | DelayBeforeShutdown | 24-bit signed, 1 s/raw unit |
| 0x11 | DelayBeforeStartup | 24-bit signed, 10 s/raw unit |
| 0x16 | RemainingCapacity + RunTimeToEmpty | actively polled |

## Web UI

The UPS monitor page contains a **Steuerung** card with:

- current shutdown timer
- current startup timer
- Shutdown in 30 s
- Shutdown abbrechen
- Startup in 300 s
- Startup abbrechen

Timer values refresh every five seconds.

## Security note

The current firmware does not implement authentication for control endpoints. Deploy the controller only on a trusted network or restrict access externally.
