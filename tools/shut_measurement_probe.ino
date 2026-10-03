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

void drain()
{
  while (mge.available())
    mge.read();
}

void printHex(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    if (i)
      Serial.print(' ');

    Serial.printf("%02X", p[i]);
  }

  Serial.println();
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

bool getReport(uint8_t id, uint8_t *out, size_t cap, size_t &n)
{
  uint8_t payload[] = {
    0xA1, 0x01, id, 0x03,
    0x00, 0x00, 0x08, 0x00
  };

  uint8_t packet[11] = {
    0x81, 0x88
  };

  memcpy(packet + 2, payload, 8);
  packet[10] = xsum(payload, 8);

  mge.write(packet, sizeof(packet));
  mge.flush();

  unsigned long start = millis();
  n = 0;

  while (millis() - start < 2500)
  {
    while (mge.available())
    {
      uint8_t type = mge.read();

      if (type == 0x06 || type == 0x15)
        continue;

      if (type != 0x04 && type != 0x84)
        continue;

      while (!mge.available() && millis() - start < 2500)
        yield();

      if (!mge.available())
        return false;

      uint8_t len = mge.read() & 0x0F;

      for (uint8_t i = 0; i < len; i++)
      {
        while (!mge.available() && millis() - start < 2500)
          yield();

        if (!mge.available())
          return false;

        uint8_t value = mge.read();

        if (n < cap)
          out[n++] = value;
      }

      while (!mge.available() && millis() - start < 2500)
        yield();

      if (mge.available())
        mge.read(); // checksum

      mge.write(0x06);
      mge.flush();

      if (type == 0x84)
        return n > 0;
    }
  }

  return false;
}

void setup()
{
  Serial.begin(115200);
  mge.begin(2400);
  delay(500);

  Serial.println("=== MGE SHUT measurement probe (read-only) ===");

  if (!sync())
  {
    Serial.println("SYNC: NONE");
    return;
  }

  Serial.println("SYNC: OK");

  const uint8_t reportIds[] = {
    0x0B, 0x0C, 0x0D, 0x0E, 0x16
  };

  for (uint8_t id : reportIds)
  {
    uint8_t response[32];
    size_t responseLen = 0;

    Serial.printf("\nREPORT 0x%02X\n", id);

    if (!getReport(id, response, sizeof(response), responseLen))
    {
      Serial.println("NONE/ERROR");
      continue;
    }

    printHex(response, responseLen);

    if (id == 0x16 && responseLen >= 4)
    {
      uint16_t runtime =
        (uint16_t)response[2] |
        ((uint16_t)response[3] << 8);

      Serial.printf(
        "capacity=%u%% runtime=%u s (%u:%02u)\n",
        response[1],
        runtime,
        runtime / 60,
        runtime % 60
      );
    }
  }
}

void loop()
{
}
