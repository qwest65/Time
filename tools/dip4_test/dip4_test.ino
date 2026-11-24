/*
  ДИАГНОСТИКА связи с DIP4-70/485
  Открыть Serial Monitor на 9600. Скетч по кругу шлёт 6 разных посылок
  (каждая держится 4 секунды) и печатает, что отправил и что ответило табло.
  Запишите, на каком ШАГЕ что-то изменилось на табло.
*/
#include <SoftwareSerial.h>

#define PIN_RS485_RX   10   // RO
#define PIN_RS485_TX   11   // DI
#define PIN_RS485_DIR  3    // DE+RE

const uint16_t ADDR = 1;
SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);

void hex(uint8_t b) { if (b < 16) Serial.print('0'); Serial.print(b, HEX); Serial.print(' '); }

void putb(uint8_t b, bool stuff) {
  if (stuff && (b == 0x02 || b == 0x03 || b == 0x09)) { rs485.write((uint8_t)0x09); hex(0x09); }
  rs485.write(b); hex(b);
}

// forceFF = true -> вместо контрольной суммы 0xFF (контроль отключён)
void sendFrame(uint16_t addr, uint8_t cmd, const uint8_t *d, uint8_t len, bool forceFF) {
  uint8_t sum = (addr >> 8) + (addr & 0xFF) + cmd;
  for (uint8_t i = 0; i < len; i++) sum += d[i];
  uint8_t crc = forceFF ? 0xFF : (uint8_t)~sum;

  Serial.print(F("  TX: "));
  digitalWrite(PIN_RS485_DIR, HIGH);
  delay(1);
  putb(0x02, false);
  putb(addr >> 8, true); putb(addr & 0xFF, true); putb(cmd, true);
  for (uint8_t i = 0; i < len; i++) putb(d[i], true);
  putb(crc, true);
  putb(0x03, false);
  delay(2);
  digitalWrite(PIN_RS485_DIR, LOW);
  Serial.println();
}

void listen(unsigned long ms) {
  unsigned long t = millis();
  bool any = false;
  while (millis() - t < ms) {
    while (rs485.available()) {
      if (!any) { Serial.print(F("  RX (ответ табло): ")); any = true; }
      hex(rs485.read());
    }
  }
  Serial.println(any ? "" : "  RX: ответа нет");
}

const uint8_t seg1234[4] = {0x06, 0x5B, 0x4F, 0x66}; // 1 2 3 4
const char    txt1234[4] = {'1', '2', '3', '4'};

void setup() {
  Serial.begin(9600);
  pinMode(PIN_RS485_DIR, OUTPUT);
  digitalWrite(PIN_RS485_DIR, LOW);
  rs485.begin(9600);
  Serial.println(F("=== Тест DIP4 ==="));
}

void loop() {
  Serial.println(F("\nШАГ 1: широковещательно ВСЁ ВКЛЮЧЕНО (0x0A) - должны загореться все сегменты"));
  sendFrame(0xFFFF, 0x0A, nullptr, 0, false); listen(4000);

  Serial.println(F("ШАГ 2: широковещательно ВОССТАНОВИТЬ (0x0D)"));
  sendFrame(0xFFFF, 0x0D, nullptr, 0, false); listen(1000);

  Serial.println(F("ШАГ 3: адрес 1, текст '1234' (0x2E)"));
  sendFrame(ADDR, 0x2E, (const uint8_t*)txt1234, 4, false); listen(4000);

  Serial.println(F("ШАГ 4: адрес 1, текст '1234' (0x2E), CRC=FF"));
  sendFrame(ADDR, 0x2E, (const uint8_t*)txt1234, 4, true); listen(4000);

  Serial.println(F("ШАГ 5: адрес 1, сегменты (0x1E)"));
  sendFrame(ADDR, 0x1E, seg1234, 4, false); listen(4000);

  Serial.println(F("ШАГ 6: адрес 1, сегменты (0x0E)"));
  sendFrame(ADDR, 0x0E, seg1234, 4, false); listen(4000);

  Serial.println(F("ШАГ 7: широковещательно сегменты (0x1E, адрес FFFF)"));
  sendFrame(0xFFFF, 0x1E, seg1234, 4, false); listen(4000);
}
