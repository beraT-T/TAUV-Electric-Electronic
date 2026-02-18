#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_SDA PB11
#define OLED_SCL PB10

// --- DONANIM AYARLARI ---
#define ESC_PIN   PA8
#define LED_PIN   PC13

// --- BUTON AYARLARI ---
#define BTN_MODE  PA4
#define BTN_UP    PC14
#define BTN_DOWN  PA7

// --- MOSFET (BTS432E2) AYARLARI ---
// PB4: MOSFET IN (Giriş) Pini -> HIGH olunca ESC'ye güç verir.
// PB5: Analog Sense Enable Pini -> Sürekli HIGH tutulur (analog sense'i aktif eder)
// PA0 (A0): MOSFET Current Sense ve Feedback Pini -> ADC ile okunur.
#define MOSFET_INPUT   PB9
#define MOSFET_ANALOG_SENSE_ENABLE PB5 // Analog sense enable pini - sürekli HIGH
#define MOSFET_CURRENT_SENSE PA0  // A0 pini - ADC ile akım okuma
#define R_SENSE 2200.0f       // Sense direnci (Oh m cinsinden)
#define K_FACTOR 16710.0f     // VN7004CH için Datasheet Typ değeri (15A civarı)
#define ADC_REF_VOLTAGE 3.3f  // Blue Pill lojik seviyesi
#define ADC_RESOLUTION 4095.0f
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

// Mode: 0 = Auto, 1 = Manual, 2 = MOSFET Test, 3 = PWM + ADC Monitor
int currentMode = 0;
int currentPwm = 1500;
static bool analogSenseEnabled = false;  // Analog sense enable durumu

// --- Fonksiyon Tanımları ---
void setThrottle(int microseconds);
void updateOLED(int pwmValue, String status);
bool checkButton(uint8_t pin);
bool checkModeChange();
void runAutoSequence();
void runManualControl();
void runMOSFETControl();
void runPWMMonitor();  // PWM + ADC Monitor modu
float readMOSFETCurrent();  // ADC ile akım okuma
int readADCValue();  // Ham ADC değerini okuma

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(ESC_PIN, OUTPUT);

  Wire.setSDA(OLED_SDA);
  Wire.setSCL(OLED_SCL);
  Wire.begin();

  // Butonlar
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // MOSFET Kontrol Pinleri
  pinMode(MOSFET_INPUT, OUTPUT);
  pinMode(MOSFET_ANALOG_SENSE_ENABLE, OUTPUT);  // PB4: Analog sense enable
  pinMode(MOSFET_CURRENT_SENSE, INPUT_ANALOG);  // A0 pini ADC olarak ayarlandı
  
  // ADC yapılandırması
  analogReadResolution(12);  // 12-bit ADC çözünürlüğü (0-4095)
  
  // Analog sense enable pinini sürekli HIGH tut (PB4)
  digitalWrite(MOSFET_ANALOG_SENSE_ENABLE, HIGH);
  analogSenseEnabled = true;
  delay(10);  // Pin'in stabilize olması için kısa bekleme
  
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
  // Analog sense enable pinini sürekli HIGH tut (PB4)
  digitalWrite(MOSFET_ANALOG_SENSE_ENABLE, HIGH);
  
  if (checkModeChange()) delay(200);

  if (currentMode == 0)      runAutoSequence();
  else if (currentMode == 1) runManualControl();
  else if (currentMode == 2) runMOSFETControl();
  else if (currentMode == 3) runPWMMonitor();
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

// --- MOD 3: PWM + ADC MONITOR ---
void runPWMMonitor() {
  // MOSFET'i AÇ (Güç ver)
  digitalWrite(MOSFET_INPUT, HIGH);
  
  // UP tuşu: PWM artır
  if (checkButton(BTN_UP)) {
    if (currentPwm < PWM_FWD_TEST) currentPwm += 3;
  }
  
  // DOWN tuşu: PWM azalt
  if (checkButton(BTN_DOWN)) {
    if (currentPwm > PWM_REV_TEST) currentPwm -= 3;
  }
  
  setThrottle(currentPwm);
  updateOLED(currentPwm, "PWM+ADC");
  
  delay(50);
}

// --- MOD DEĞİŞTİRME ---
bool checkModeChange() {
  if (checkButton(BTN_MODE)) {
    currentMode = (currentMode + 1) % 4; // 0->1->2->3->0
    currentPwm = 1500;

    setThrottle(1500); // Motoru durdur
    
    // Mod değiştirirken MOSFET'i kapat
    digitalWrite(MOSFET_INPUT, LOW); 
    
    String modeName;
    if (currentMode == 0) modeName = "OTOMATIK";
    else if (currentMode == 1) modeName = "MANUEL";
    else if (currentMode == 2) modeName = "MOSFET TEST";
    else modeName = "PWM+ADC";
    
    updateOLED(1500, modeName);

    while (digitalRead(BTN_MODE) == LOW); // Butonu bırakana kadar bekle
    return true;
  }
  return false;
}

void setThrottle(int microseconds) {
  ESCTimer->setCaptureCompare(1, microseconds, MICROSEC_COMPARE_FORMAT);
}

// --- ADC ile MOSFET Akım Okuma ---
int readADCValue() {
  // Analog sense enable pinini HIGH tut (her okumadan önce)
  digitalWrite(MOSFET_ANALOG_SENSE_ENABLE, HIGH);
  delayMicroseconds(10);  // Kısa bir bekleme
  
  // A0 pininden ham ADC değerini oku (0-4095 arası, 12-bit ADC)
  int adcValue = analogRead(MOSFET_CURRENT_SENSE);
  
  return adcValue;
}
float readMOSFETCurrent() {
  // 1. ADC Değerini Oku (0 - 4095)
  int adcValue = readADCValue();
  
  // 2. ADC'yi Voltaja Çevir (CS Pinindeki Voltaj)
  // Örn: 2445 okursanız -> 1.97V
  float voltage = (adcValue * ADC_REF_VOLTAGE) / ADC_RESOLUTION;
  
  // 3. Sense Akımını Hesapla (Isense = V / R)
  // Örn: 1.97V / 2200R = 0.000895A (0.895mA)
  float i_sense = voltage / R_SENSE;
  
  // 4. Ana Yük Akımını Hesapla (Iout = Isense * K)
  // Örn: 0.000895 * 16710 = ~14.96A
  float current = i_sense * K_FACTOR;
  
  return current;
}

void updateOLED(int pwmValue, String status) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  
  if (currentMode == 0) display.print("OTOMATIK");
  else if (currentMode == 1) display.print("MANUEL");
  else if (currentMode == 2) display.print("MOSFET TEST");
  else display.print("PWM+ADC");

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
  
  // ADC ve akım değerlerini göster
  int adcRaw = readADCValue();
  float current = readMOSFETCurrent();
  
  if (currentMode == 3) {
    // MOD 3: Detaylı ADC bilgileri
    display.setCursor(0, 20);
    display.print("ADC: ");
    display.print(adcRaw);
    
    display.setCursor(70, 20);
    display.print("PWM: ");
    display.print(pwmValue);
    
    display.setCursor(0, 30);
    display.print("I: ");
    display.print(current, 3);
    display.print("A");
    
    display.setCursor(70, 30);
    display.print("V: ");
    float voltage = (adcRaw * 3.3) / 4095.0;
    display.print(voltage, 2);
    display.print("V");
  } else {
    // Diğer modlar: Sadece akım
    display.setCursor(0, 20);
    display.print("I: ");
    display.print(current, 3);
    display.print("A");
    
    // Bar grafiği (MOD 3 hariç)
    display.drawRect(0, 32, 128, 10, SSD1306_WHITE);
    display.drawLine(64, 32, 64, 42, SSD1306_WHITE);

    if (pwmValue >= 1500) {
      int barWidth = map(pwmValue, 1500, PWM_FWD_TEST, 0, 62);
      if (barWidth > 62) barWidth = 62;
      display.fillRect(64, 34, barWidth, 6, SSD1306_WHITE);
    } else {
      int barWidth = map(pwmValue, 1500, PWM_REV_TEST, 0, 62);
      if (barWidth > 62) barWidth = 62;
      display.fillRect(64 - barWidth, 34, barWidth, 6, SSD1306_WHITE);
    }
  }

  display.display();
}