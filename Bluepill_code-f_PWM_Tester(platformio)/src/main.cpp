#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- DONANIM AYARLARI ---
#define ESC_PIN   PA8
#define LED_PIN   PC13

// --- BUTON AYARLARI ---
#define BTN_MODE  PA9
#define BTN_UP    PA10
#define BTN_DOWN  PA11

// --- MOSFET (BTS432E2) AYARLARI ---
// PB0: BTS432 IN (Giriş) Pini -> HIGH olunca ESC'ye güç verir.
// PB1: BTS432 ST (Status) Pini -> Hata durumunda LOW olur.
#define MOSFET_INPUT   PB0
#define MOSFET_FEEDBACK PB1

// --- OLED AYARLARI ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

// --- LİMİTLER ---
#define PWM_NEUTRAL  1500
#define PWM_FWD_TEST 1702
#define PWM_REV_TEST 1396
#define PIXHAWK_FREQ 200

// --- GLOBAL DEĞİŞKENLER ---
HardwareTimer *ESCTimer = new HardwareTimer(TIM1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Mode: 0 = Auto, 1 = Manual, 2 = MOSFET Test
int currentMode = 0;
int currentPwm = 1500;

// --- Fonksiyon Tanımları ---
void setThrottle(int microseconds);
void updateOLED(int pwmValue, String status);
bool checkButton(uint8_t pin);
bool checkModeChange();
void runAutoSequence();
void runManualControl();
void runMOSFETControl();

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(ESC_PIN, OUTPUT);

  // Butonlar
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // MOSFET Kontrol Pinleri
  pinMode(MOSFET_INPUT, OUTPUT);
  pinMode(MOSFET_FEEDBACK, INPUT_PULLUP);
  
  // Başlangıçta MOSFET KAPALI olsun (Güvenlik)
  digitalWrite(MOSFET_INPUT, LOW);

  // OLED Başlat
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    while(true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- TIMER KUR (TIM1) ---
  ESCTimer->setOverflow(PIXHAWK_FREQ, HERTZ_FORMAT);
  ESCTimer->setMode(1, TIMER_OUTPUT_COMPARE_PWM1, ESC_PIN);
  ESCTimer->setCaptureCompare(1, 1500, MICROSEC_COMPARE_FORMAT);
  ESCTimer->resume();
  TIM1->BDTR |= TIM_BDTR_MOE; // Main Output Enable (Kritik)

  updateOLED(1500, "HAZIR");
  delay(500);
}

void loop() {
  if (checkModeChange()) delay(200);

  if (currentMode == 0)      runAutoSequence();
  else if (currentMode == 1) runManualControl();
  else if (currentMode == 2) runMOSFETControl();
}

// --- Debounce Fonksiyonu ---
bool checkButton(uint8_t pin) {
  if (digitalRead(pin) == LOW) {
    delay(25); // Debounce
    return (digitalRead(pin) == LOW);
  }
  return false;
}

// --- MOD 1: MANUAL KONTROL ---
void runManualControl() {
  // MOSFET'i AÇ (Manuel modda güç ver)
  digitalWrite(MOSFET_INPUT, HIGH);

  if (checkButton(BTN_UP)) {
    if (currentPwm < PWM_FWD_TEST) currentPwm += 3;
  }

  if (checkButton(BTN_DOWN)) {
    if (currentPwm > PWM_REV_TEST) currentPwm -= 3;
  }

  setThrottle(currentPwm);
  updateOLED(currentPwm, "MANUEL");
}

// --- MOD 0: OTO TEST ---
void runAutoSequence() {
  // Test başlarken MOSFET'i AÇ (Güç Ver)
  digitalWrite(MOSFET_INPUT, HIGH);
  
  // -- İLERİ HIZLANMA --
  for (int pwm = PWM_NEUTRAL; pwm <= PWM_FWD_TEST; pwm += 2) {
    if (checkModeChange()) return;
    
    setThrottle(pwm);
    updateOLED(pwm, "AUTO: ILERI");
    delay(10); // Döngü hızı
  }

  // -- BEKLEME --
  for (int i=0; i<25; i++) {
    if (checkModeChange()) return;
    delay(50);
  }

  // -- DURMA --
  for (int pwm = PWM_FWD_TEST; pwm >= PWM_NEUTRAL; pwm -= 5) {
    if (checkModeChange()) return;
    setThrottle(pwm);
    updateOLED(pwm, "AUTO: DUR");
    delay(10);
  }
  delay(300);

  // -- GERİ HIZLANMA --
  for (int pwm = PWM_NEUTRAL; pwm >= PWM_REV_TEST; pwm -= 2) {
    if (checkModeChange()) return;
    
    setThrottle(pwm);
    updateOLED(pwm, "AUTO: GERI");
    delay(10);
  }

  // -- BEKLEME --
  for (int i=0; i<25; i++) {
    if (checkModeChange()) return;
    delay(50);
  }

  // -- DURMA --
  for (int pwm = PWM_REV_TEST; pwm <= PWM_NEUTRAL; pwm += 5) {
    if (checkModeChange()) return;
    setThrottle(pwm);
    updateOLED(pwm, "AUTO: DUR");
    delay(10);
  }
  delay(300);
}

// --- MOD 2: MOSFET TEST (Yük Altında Kesme Testi) ---
void runMOSFETControl() {
  static bool mosfetState = false;
  
  // UP tuşu: MOSFET AÇ (Güç ver)
  if (checkButton(BTN_UP)) {
    mosfetState = true;
    digitalWrite(MOSFET_INPUT, HIGH);
  }
  
  // DOWN tuşu: MOSFET KAPAT (Gücü Kes - Yük Testi için)
  if (checkButton(BTN_DOWN)) {
    mosfetState = false;
    digitalWrite(MOSFET_INPUT, LOW);
  }
  
  String status = mosfetState ? "AKTIF" : "KAPALI";
  
  // MOSFET testi yaparken motorun dönmesi lazım ki yük oluşsun.
  // Eğer MOSFET açıksa 1700us (Hafif gaz) gönder, kapalıysa 1500us (Dur) gönder.
  setThrottle(mosfetState ? 1700 : 1500);
  
  updateOLED(mosfetState ? 1700 : 1500, status);
  
  delay(50); 
}

// --- MOD DEĞİŞTİRME ---
bool checkModeChange() {
  if (checkButton(BTN_MODE)) {
    currentMode = (currentMode + 1) % 3; // 0->1->2->0
    currentPwm = 1500;

    setThrottle(1500); // Motoru durdur
    
    // Mod değiştirirken MOSFET'i kapat
    digitalWrite(MOSFET_INPUT, LOW); 
    
    String modeName;
    if (currentMode == 0) modeName = "OTOMATIK";
    else if (currentMode == 1) modeName = "MANUEL";
    else modeName = "MOSFET TEST";
    
    updateOLED(1500, modeName);

    while (digitalRead(BTN_MODE) == LOW); // Butonu bırakana kadar bekle
    return true;
  }
  return false;
}

void setThrottle(int microseconds) {
  ESCTimer->setCaptureCompare(1, microseconds, MICROSEC_COMPARE_FORMAT);
}

void updateOLED(int pwmValue, String status) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  
  if (currentMode == 0) display.print("OTOMATIK");
  else if (currentMode == 1) display.print("MANUEL");
  else display.print("MOSFET TEST");

  display.setCursor(80, 0);
  if (currentMode == 2) {
    display.print(digitalRead(MOSFET_INPUT) ? "ON" : "OFF");
  } else {
    display.print(pwmValue);
    display.print("us");
  }

  display.setCursor(0, 10);
  display.print("Drm: ");
  display.print(status);

  display.drawRect(0, 22, 128, 10, SSD1306_WHITE);
  display.drawLine(64, 22, 64, 32, SSD1306_WHITE);

  if (pwmValue >= 1500) {
    int barWidth = map(pwmValue, 1500, PWM_FWD_TEST, 0, 62);
    if (barWidth > 62) barWidth = 62;
    display.fillRect(64, 24, barWidth, 6, SSD1306_WHITE);
  } else {
    int barWidth = map(pwmValue, 1500, PWM_REV_TEST, 0, 62);
    if (barWidth > 62) barWidth = 62;
    display.fillRect(64 - barWidth, 24, barWidth, 6, SSD1306_WHITE);
  }

  display.display();
}