#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

// --- Pin Tanımlamaları ---
#define ETH_CS_PIN  PA15
#define ETH_RST_PIN PA3
#define LAZER_PIN   PB9  // TC4426 Kanal A (Inverting)
#define TRAFO_PIN   PB8  // TC4426 Kanal B (Inverting)
#define LED_PWM_PIN PB7  // A6211 EN Pin (Direct PWM)

// --- Ağ Yapılandırması ---
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
IPAddress ip(192, 168, 1, 20);
unsigned int localPort = 5000;

EthernetUDP Udp;
char packetBuffer[UDP_TX_PACKET_MAX_SIZE];

void setup() {
  Serial.begin(115200);

  // 1. MOSFET ve LED Pin Başlatma
  pinMode(LAZER_PIN, OUTPUT);
  pinMode(TRAFO_PIN, OUTPUT);
  pinMode(LED_PWM_PIN, OUTPUT);

  // TC4426 Inverting: HIGH = MOSFET OFF
  digitalWrite(LAZER_PIN, HIGH);
  digitalWrite(TRAFO_PIN, HIGH);
  analogWrite(LED_PWM_PIN, 0); // A6211 OFF

  // 2. Ethernet Donanım Reset
  pinMode(ETH_RST_PIN, OUTPUT);
  pinMode(ETH_CS_PIN, OUTPUT);
  digitalWrite(ETH_CS_PIN, HIGH);
  digitalWrite(ETH_RST_PIN, LOW);
  delay(200);
  digitalWrite(ETH_RST_PIN, HIGH);
  delay(500);

  // 3. SPI ve Ethernet Başlatma
  SPI.setMOSI(PB5);
  SPI.setMISO(PB4);
  SPI.setSCLK(PB3);
  SPI.begin();
  Ethernet.init(ETH_CS_PIN);
  Ethernet.begin(mac, ip, IPAddress(8,8,8,8), IPAddress(192,168,1,1), IPAddress(255,255,255,0));
  Udp.begin(localPort);

  // A6211 PWM Frekansı Ayarı (1kHz)
  analogWriteFrequency(1000); 

  Serial.println("AUV Guc Sistemi Hazir. UDP Port: 5000");
}

void loop() {
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    int len = Udp.read(packetBuffer, UDP_TX_PACKET_MAX_SIZE);
    if (len > 0) packetBuffer[len] = 0;
    String cmd = String(packetBuffer);
    cmd.trim();

    String cmdId = "";
    String actualCmd = cmd;
    String response = "ERR";

    // --- ID Parse Etme ---
    int colonIndex = cmd.indexOf(':');
    if (colonIndex > 0) {
      cmdId = cmd.substring(0, colonIndex);
      actualCmd = cmd.substring(colonIndex + 1);
    }

    // --- Komut Ayrıştırma ---
    if (actualCmd == "L1") { // Lazer ON
      digitalWrite(LAZER_PIN, LOW); 
      response = "LAZER_ON";
    } else if (actualCmd == "L0") { // Lazer OFF
      digitalWrite(LAZER_PIN, HIGH);
      response = "LAZER_OFF";
    } else if (actualCmd == "T1") { // Trafo ON
      digitalWrite(TRAFO_PIN, LOW);
      response = "TRAFO_ON";
    } else if (actualCmd == "T0") { // Trafo OFF
      digitalWrite(TRAFO_PIN, HIGH);
      response = "TRAFO_OFF";
    } else if (actualCmd.startsWith("LED-")) { // LED Parlaklık (LED-50 gibi)
      int val = actualCmd.substring(4).toInt();
      val = constrain(val, 0, 100); // 0-100 arası sınırla
      int pwmVal = map(val, 0, 100, 0, 255); // 0-255 PWM değerine çevir
      analogWrite(LED_PWM_PIN, pwmVal);
      response = "LED_VAL_" + String(val);
    }

    // ACK'ye ID ekle (eğer komutta ID varsa)
    String finalResponse = response;
    if (cmdId.length() > 0) {
      finalResponse = cmdId + ":" + response;
    }

    // Geri Bildirim Gönder
    Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
    Udp.print(finalResponse);
    Udp.endPacket();
    
    Serial.println("Gelen: " + cmd + " -> Yanit: " + finalResponse);
  }
}