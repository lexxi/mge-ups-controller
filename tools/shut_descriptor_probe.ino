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

bool getDescriptor(
  uint8_t descriptorType,
  uint16_t wanted,
  uint8_t *out,
  size_t capacity,
  size_t &received)
{
  // USB/HID GET_DESCRIPTOR wrapped in the confirmed SHUT request format.
  uint8_t payload[8] = {
    0x81, 0x06,
    0x00, descriptorType,
    0x00, 0x00,
    (uint8_t)wanted,
    (uint8_t)(wanted >> 8)
  };

  uint8_t packet[11] = {
    0x81, 0x88
  };

  memcpy(packet + 2, payload, sizeof(payload));
  packet[10] = xsum(payload, sizeof(payload));

  mge.write(packet, sizeof(packet));
  mge.flush();

  unsigned long start = millis();
  received = 0;

  while (millis() - start < 20000)
  {
    while (mge.available())
    {
      uint8_t type = mge.read();

      // Standalone ACK from the UPS.
      if (type == 0x06)
        continue;

      if (type != 0x04 && type != 0x84)
        continue;

      while (!mge.available() && millis() - start < 20000)
        yield();

      if (!mge.available())
        return false;

      uint8_t len = mge.read() & 0x0F;

      uint8_t fragment[8];

      if (len > sizeof(fragment))
        return false;

      for (uint8_t i = 0; i < len; i++)
      {
        while (!mge.available() && millis() - start < 20000)
          yield();

        if (!mge.available())
          return false;

        fragment[i] = mge.read();

        if (received < capacity)
          out[received++] = fragment[i];
      }

      while (!mge.available() && millis() - start < 20000)
        yield();

      if (!mge.available())
        return false;

      uint8_t checksum = mge.read();

      if (xsum(fragment, len) != checksum)
        return false;

      // ACK every fragment before the UPS sends the next one.
      mge.write(0x06);
      mge.flush();

      if (type == 0x84)
        return received >= wanted;
    }

    yield();
  }

  return false;
}

void setup()
{
  Serial.begin(115200);
  mge.begin(2400);
  delay(500);

  Serial.println("=== MGE SHUT HID descriptor probe ===");

  if (!sync())
  {
    Serial.println("SYNC: NONE");
    return;
  }

  Serial.println("SYNC: OK");

  // HID descriptor: 9 bytes.
  uint8_t hid[16];
  size_t hidLen = 0;

  if (!getDescriptor(0x21, 9, hid, sizeof(hid), hidLen))
  {
    Serial.println("HID descriptor: ERROR");
    return;
  }

  Serial.print("HID descriptor: ");
  hex(hid, hidLen);

  if (hidLen < 9)
    return;

  uint16_t reportLength =
    (uint16_t)hid[7] |
    ((uint16_t)hid[8] << 8);

  Serial.printf(
    "Report descriptor length: %u bytes\n",
    reportLength
  );

  if (reportLength > 600)
  {
    Serial.println("Report descriptor too large for test buffer");
    return;
  }

  uint8_t report[600];
  size_t reportLen = 0;

  if (!getDescriptor(
        0x22,
        reportLength,
        report,
        sizeof(report),
        reportLen))
  {
    Serial.println("Report descriptor: ERROR");
    return;
  }

  Serial.printf(
    "Report descriptor assembled: %u bytes\n",
    (unsigned)reportLen
  );

  hex(report, reportLen);
}

void loop()
{
}
