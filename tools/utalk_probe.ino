#include <Arduino.h>
#include <SoftwareSerial.h>

#define MGE_RX_PIN D5
#define MGE_TX_PIN D6

SoftwareSerial mge(MGE_RX_PIN, MGE_TX_PIN);

void drain()
{
  while (mge.available())
    mge.read();
}

void probe(const char *label, const uint8_t *data, size_t len)
{
  drain();

  Serial.printf("\nTX %s: ", label);
  for (size_t i = 0; i < len; i++)
  {
    mge.write(data[i]);
    Serial.printf("%02X ", data[i]);
  }

  mge.flush();

  unsigned long start = millis();
  bool any = false;

  Serial.print("\nRX: ");

  while (millis() - start < 1200)
  {
    while (mge.available())
    {
      uint8_t b = mge.read();
      Serial.printf("%02X ", b);
      any = true;
    }

    yield();
  }

  if (!any)
    Serial.print("NONE");

  Serial.println();
}

void setup()
{
  Serial.begin(115200);
  mge.begin(2400);
  delay(500);

  Serial.println("=== MGE U-Talk probe (read-only) ===");

  const uint8_t z[] = {'Z', '\r', '\n'};
  const uint8_t si1[] = {'S', 'i', ' ', '1', '\r', '\n'};
  const uint8_t si[] = {'S', 'i', '\r', '\n'};

  probe("Z", z, sizeof(z));
  probe("Si 1", si1, sizeof(si1));
  probe("Si", si, sizeof(si));
}

void loop()
{
}
