#include <Arduino.h>
#include <SoftwareSerial.h>

#define MGE_RX_PIN D5
#define MGE_TX_PIN D6

SoftwareSerial mge(MGE_RX_PIN, MGE_TX_PIN);

uint8_t xsum(const uint8_t *p, size_t n)
{
  uint8_t x = 0;
  while (n--)
    x ^= *p++;
  return x;
}

void hex(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    if (i)
      Serial.print(' ');

    Serial.printf("%02X", p[i]);
  }

  Serial.println();
}

void drain()
{
  while (mge.available())
    mge.read();
}

bool sync()
{
  drain();

  mge.write(0x16);
  mge.flush();

  unsigned long start = millis();

  while (millis() - start < 1000)
  {
    if (mge.available() && mge.read() == 0x16)
      return true;

    yield();
  }

  return false;
}

void getReport(uint8_t id)
{
  // Confirmed SHUT GET REPORT request:
  // 81 88 A1 01 <ReportID> 03 00 00 08 00 <checksum>
  uint8_t payload[] = {
    0xA1, 0x01, id, 0x03,
    0x00, 0x00, 0x08, 0x00
  };

  uint8_t packet[11] = {
    0x81, 0x88
  };

  memcpy(packet + 2, payload, 8);
  packet[10] = xsum(payload, 8);

  Serial.print("TX: ");
  hex(packet, sizeof(packet));

  mge.write(packet, sizeof(packet));
  mge.flush();

  unsigned long start = millis();
  uint8_t out[64];
  size_t outLen = 0;

  while (millis() - start < 2500)
  {
    while (mge.available())
    {
      uint8_t type = mge.read();

      if (type == 0x06)
      {
        Serial.println("ACK 06");
        continue;
      }

      if (type == 0x15)
      {
        Serial.println("NOK 15");
        continue;
      }

      if (type != 0x04 && type != 0x84)
        continue;

      while (!mge.available() && millis() - start < 2500)
        yield();

      if (!mge.available())
        continue;

      uint8_t lenByte = mge.read();
      uint8_t len = lenByte & 0x0F;

      for (uint8_t i = 0; i < len && outLen < sizeof(out); i++)
      {
        while (!mge.available() && millis() - start < 2500)
          yield();

        if (mge.available())
          out[outLen++] = mge.read();
      }

      while (!mge.available() && millis() - start < 2500)
        yield();

      if (mge.available())
        mge.read(); // checksum

      mge.write(0x06);
      mge.flush();

      if (type == 0x84)
      {
        Serial.print("RX: ");
        hex(out, outLen);
        return;
      }
    }

    yield();
  }

  Serial.println("RX: TIMEOUT");
}

void setup()
{
  Serial.begin(115200);
  mge.begin(2400);
  delay(500);

  Serial.println("=== MGE SHUT GET REPORT probe ===");

  if (!sync())
  {
    Serial.println("SYNC: NONE");
    return;
  }

  Serial.println("SYNC: OK");
  getReport(0x16);
}

void loop()
{
}
