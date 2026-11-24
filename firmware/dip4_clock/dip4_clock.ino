/*
  Часы + термометр на табло DIP4-70/485
  Arduino (Uno/Nano) + TTL-RS485 + DS3231 + DS18B20

  Подключение (по схеме):
    RS485 RO  -> D10  (приём)
    RS485 DI  -> D11  (передача)
    RS485 DE+RE -> D3 (направление: HIGH = передача)
    RS485 A/B -> A/B табло
    DS18B20 DATA -> D2  (+ резистор 4.7 кОм между DATA и +5V!)
    DS3231 SDA -> A4, SCL -> A5

  Библиотеки (Менеджер библиотек):
    RTClib (Adafruit), OneWire, DallasTemperature

  Работа: 25 с показывается время "ЧЧ.ММ" (точка мигает раз в секунду),
          затем 5 с температура "23.5°" (или "-12°").

  Ручная подводка часов кнопкой на D12 (кнопка между D12 и GND):
    1. Держать 5 с  -> режим установки ЧАСОВ (часы мигают),
                       короткое нажатие = +1 час
    2. Держать 5 с  -> режим установки МИНУТ (минуты мигают),
                       короткое нажатие = +1 минута
    3. Держать 5 с или ничего не нажимать 10 с -> сохранение и выход
       (секунды обнуляются в момент сохранения)

  Установка времени через Serial Monitor (9600, "Новая строка"):
    set 2026 11 24 15 30 00
*/

#include <Wire.h>
#include <SoftwareSerial.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- Настройки ----------------
#define PIN_RS485_RX   10      // RO
#define PIN_RS485_TX   11      // DI
#define PIN_RS485_DIR  3       // DE + RE
#define PIN_DS18B20    2
#define PIN_BUTTON     12      // кнопка подводки (на GND)

const unsigned long LONG_PRESS_MS = 5000;  // длительное нажатие
const unsigned long SET_TIMEOUT_MS = 10000; // выход из настройки при бездействии

const uint16_t MODULE_ADDR = 1;   // адрес табло (смотрится в режиме настройки 3/4 кнопкой SW1)
const uint8_t  BRIGHTNESS  = 100; // яркость 0..125

const unsigned long TIME_SHOW_MS = 25000; // сколько показывать время
const unsigned long TEMP_SHOW_MS = 5000;  // сколько показывать температуру

// Если цифры на табло выводятся задом наперёд — поменяйте на false
#define REVERSE_DIGITS true
// -------------------------------------------

SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);
RTC_DS3231 rtc;
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// Сегменты (команда 0x1E): бит0=a(верх) 1=b 2=c 3=d(низ) 4=e 5=f 6=g(середина) 7=точка
const uint8_t DIGITS[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
const uint8_t SEG_MINUS  = 0x40;
const uint8_t SEG_DEGREE = 0x63;
const uint8_t SEG_BLANK  = 0x00;
const uint8_t SEG_DP     = 0x80;

enum Mode { MODE_NORMAL, MODE_SET_HOUR, MODE_SET_MIN };
Mode mode = MODE_NORMAL;
uint8_t setHour = 0, setMin = 0;
unsigned long lastActivity = 0;

float temperature = NAN;
unsigned long lastTempRequest = 0;
bool tempPending = false;

// ---------- Протокол DIP4 ----------
void sendStuffed(uint8_t b) {
  if (b == 0x02 || b == 0x03 || b == 0x09) rs485.write((uint8_t)0x09);
  rs485.write(b);
}

void sendFrame(uint16_t addr, uint8_t cmd, const uint8_t *data, uint8_t len) {
  uint8_t sum = 0;
  uint8_t adh = addr >> 8, adl = addr & 0xFF;
  sum += adh; sum += adl; sum += cmd;
  for (uint8_t i = 0; i < len; i++) sum += data[i];
  uint8_t crc = ~sum;

  digitalWrite(PIN_RS485_DIR, HIGH);    // передача
  delayMicroseconds(100);
  rs485.write((uint8_t)0x02);
  sendStuffed(adh);
  sendStuffed(adl);
  sendStuffed(cmd);
  for (uint8_t i = 0; i < len; i++) sendStuffed(data[i]);
  sendStuffed(crc);
  rs485.write((uint8_t)0x03);
  delayMicroseconds(200);               // дождаться выхода последнего стоп-бита
  digitalWrite(PIN_RS485_DIR, LOW);     // приём (квитанция)
}

// seg[0] — левый разряд, seg[3] — правый
void showSegments(const uint8_t seg[4]) {
  uint8_t d[4];
  for (uint8_t i = 0; i < 4; i++)
    d[i] = REVERSE_DIGITS ? seg[3 - i] : seg[i];
  sendFrame(MODULE_ADDR, 0x1E, d, 4);
}

void setBrightness(uint8_t level) {
  if (level > 125) level = 125;
  sendFrame(0xFFFF, 0x17, &level, 1);   // широковещательная
}

// ---------- Экраны ----------
void showTime(const DateTime &now) {
  uint8_t s[4];
  s[0] = DIGITS[now.hour() / 10];
  s[1] = DIGITS[now.hour() % 10];
  s[2] = DIGITS[now.minute() / 10];
  s[3] = DIGITS[now.minute() % 10];
  if (now.second() % 2 == 0) s[1] |= SEG_DP;   // мигающий разделитель
  showSegments(s);
}

void showTemperature() {
  uint8_t s[4];
  if (isnan(temperature)) {                    // датчик не найден: "---°"
    s[0] = s[1] = s[2] = SEG_MINUS; s[3] = SEG_DEGREE;
    showSegments(s);
    return;
  }
  int t10 = (int)round(temperature * 10);      // десятые доли
  bool neg = t10 < 0;
  int a = abs(t10);

  if (a < 100) {                 // -9.9 .. 9.9  -> " 5.3°" / "-5.3°"
    s[0] = neg ? SEG_MINUS : SEG_BLANK;
    s[1] = DIGITS[a / 10] | SEG_DP;
    s[2] = DIGITS[a % 10];
  } else if (!neg && a < 1000) { // 10.0 .. 99.9 -> "23.5°"
    s[0] = DIGITS[a / 100];
    s[1] = DIGITS[(a / 10) % 10] | SEG_DP;
    s[2] = DIGITS[a % 10];
  } else {                       // -10 и ниже -> "-12°", 100+ -> "105°"
    int whole = (a + 5) / 10;
    if (whole > 999) whole = 999;
    s[0] = neg ? SEG_MINUS : (whole >= 100 ? DIGITS[whole / 100] : SEG_BLANK);
    s[1] = DIGITS[(whole / 10) % 10];
    s[2] = DIGITS[whole % 10];
  }
  s[3] = SEG_DEGREE;
  showSegments(s);
}

// Экран настройки: редактируемая пара цифр мигает, точка горит постоянно
void showSetting() {
  bool blinkOff = (millis() / 300) % 2;        // мигание ~1.7 раза в секунду
  uint8_t s[4];
  s[0] = DIGITS[setHour / 10];
  s[1] = DIGITS[setHour % 10] | SEG_DP;
  s[2] = DIGITS[setMin / 10];
  s[3] = DIGITS[setMin % 10];
  if (blinkOff) {
    if (mode == MODE_SET_HOUR) { s[0] = SEG_BLANK; s[1] = SEG_DP; }
    else                       { s[2] = SEG_BLANK; s[3] = SEG_BLANK; }
  }
  showSegments(s);
}

void saveTime() {
  DateTime n = rtc.now();
  rtc.adjust(DateTime(n.year(), n.month(), n.day(), setHour, setMin, 0));
  mode = MODE_NORMAL;
  Serial.println(F("Время сохранено"));
}

// ---------- Кнопка подводки ----------
void handleButton() {
  static bool lastRaw = HIGH, stable = HIGH;
  static unsigned long changeTime = 0, pressStart = 0;
  static bool longDone = false;

  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastRaw) { lastRaw = raw; changeTime = millis(); }
  if (millis() - changeTime < 30) return;      // антидребезг

  if (raw != stable) {
    stable = raw;
    if (stable == LOW) {                       // нажали
      pressStart = millis();
      longDone = false;
    } else {                                   // отпустили
      if (!longDone && millis() - pressStart < 1000) {   // короткое нажатие
        if (mode == MODE_SET_HOUR) setHour = (setHour + 1) % 24;
        if (mode == MODE_SET_MIN)  setMin  = (setMin + 1) % 60;
      }
      lastActivity = millis();
    }
  }

  // Длительное удержание — срабатывает сразу по истечении 5 с, не дожидаясь отпускания
  if (stable == LOW && !longDone && millis() - pressStart >= LONG_PRESS_MS) {
    longDone = true;
    lastActivity = millis();
    if (mode == MODE_NORMAL) {
      DateTime n = rtc.now();
      setHour = n.hour();
      setMin = n.minute();
      mode = MODE_SET_HOUR;
    } else if (mode == MODE_SET_HOUR) {
      mode = MODE_SET_MIN;
    } else {
      saveTime();
    }
  }

  // Выход по бездействию
  if (mode != MODE_NORMAL && stable == HIGH && millis() - lastActivity >= SET_TIMEOUT_MS) {
    saveTime();
  }
}

// ---------- Температура без блокировки ----------
void updateTemperature() {
  unsigned long now = millis();
  if (!tempPending && now - lastTempRequest >= 2000) {
    sensors.requestTemperatures();
    lastTempRequest = now;
    tempPending = true;
  }
  if (tempPending && now - lastTempRequest >= 800) {  // 12 бит = 750 мс
    float t = sensors.getTempCByIndex(0);
    temperature = (t == DEVICE_DISCONNECTED_C) ? NAN : t;
    tempPending = false;
  }
}

// ---------- Установка времени из Serial ----------
void checkSerial() {
  static char buf[40];
  static uint8_t pos = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[pos] = 0;
      int Y, M, D, h, m, s;
      if (pos && sscanf(buf, "set %d %d %d %d %d %d", &Y, &M, &D, &h, &m, &s) == 6) {
        rtc.adjust(DateTime(Y, M, D, h, m, s));
        Serial.println(F("Время установлено"));
      }
      pos = 0;
    } else if (pos < sizeof(buf) - 1) {
      buf[pos++] = c;
    }
  }
}

void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RS485_DIR, OUTPUT);
  digitalWrite(PIN_RS485_DIR, LOW);
  rs485.begin(9600);

  if (!rtc.begin()) Serial.println(F("DS3231 не найден!"));
  if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);

  delay(500);
  setBrightness(BRIGHTNESS);
  Serial.println(F("Готово. Установка времени: set ГГГГ ММ ДД чч мм сс"));
}

void loop() {
  checkSerial();
  handleButton();
  updateTemperature();
  while (rs485.available()) rs485.read();      // квитанции табло не нужны

  static unsigned long lastSend = 0;
  static Mode lastMode = MODE_NORMAL;
  // в настройке обновляем чаще и сразу при смене режима — чтобы отклик был мгновенный
  unsigned long period = (mode == MODE_NORMAL) ? 250 : 100;
  if (millis() - lastSend >= period || mode != lastMode) {
    lastSend = millis();
    lastMode = mode;
    if (mode != MODE_NORMAL) {
      showSetting();
    } else {
      unsigned long phase = millis() % (TIME_SHOW_MS + TEMP_SHOW_MS);
      if (phase < TIME_SHOW_MS) showTime(rtc.now());
      else                      showTemperature();
    }
  }
}
