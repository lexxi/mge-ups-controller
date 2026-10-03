# MGE UPS controller – research tools

Diese Tools gehören **nicht** zur produktiven Monitoring-App. Sie dokumentieren die Tests, mit denen das serielle MGE-SHUT-Protokoll der konkreten USV untersucht wurde.

Hardware: ESP8266/LOLIN, 2400 8N1, MGE RX=D5, TX=D6.

## Tools

- `utalk_probe.ino` – prüft den älteren U-Talk-Kommandokanal (`Z`, `Si 1`, `Si`). Auf unserer USV gab es darauf keine Antwort. Deshalb nicht Bestandteil der Monitoring-App.
- `shut_probe.ino` – einfacher read-only SHUT GET-REPORT-Test. Abfrage des bestätigten Reports `0x16`.
- `shut_measurement_probe.ino` – read-only Scanner für `0x0B`, `0x0C`, `0x0D`, `0x0E` und `0x16`.
- `shut_descriptor_probe.ino` – liest den HID-Descriptor (`0x21`) und anschließend den HID-Report-Descriptor (`0x22`) fragmentiert aus. Jedes Fragment wird geprüft und mit `0x06` bestätigt.

## Bisher bestätigte Erkenntnisse

- SHUT SYNC: `0x16` → USV antwortet mit `0x16`.
- GET REPORT funktioniert aktiv.
- Report `0x16): `RemainingCapacity` + `RunTimeToEmpty`.
- Beispiel: `16 64 C3 05` → 100 %, 1475 s.
- Der HID Report Descriptor der konkreten USV ist 532 Bytes lang.
- `0x0E` reagiert zustandsabhängig und ist als Voltage-Usage im Descriptor vorhanden; die genaue Skalierung wird noch untersucht.
- Keine SET-REPORTs oder Shutdown-Befehle werden von diesen Tools ausgeführt.

Die Tools sind bewusst als Labor-/Reverse-Engineering-Hilfsmittel getrennt von der eigentlichen Monitoring-App.
