# Hardware

## ESP8266

Target board: LOLIN/WEMOS D1 mini (ESP8266).

## UPS serial connection

Current test sketch uses:

- D5: RX
- D6: TX
- 2400 baud
- 8 data bits
- no parity
- 1 stop bit

The UPS interface is intended to be isolated through an ADuM1201.

## Flashing

For the current LOLIN/CH340 setup, the reproducible flashing procedure uses the NodeMCU Firmware Programmer:

- select the generated Arduino .bin
- address 0x00000
- 57600 baud
- 4 MByte flash
- 40 MHz
- DIO
- put the ESP8266 into bootloader manually with FLASH + RST
- start Flash(E) while the board is in the bootloader

Do not run another serial monitor on COM3 while flashing.
