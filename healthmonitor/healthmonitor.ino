/* ==========================================================
   SMART HEALTH MONITOR - BALANCED & FAST EDITION
   - Hardware: Arduino Mega 2560
   - WiFi: ESP8266-01 (Serial1)
   - Fix: Resolved Sensor Saturation (No BPM) + Low BPM Fix
   ========================================================== */

// --- 1. BLYNK CONFIGURATION ---
// Blynk token and WiFi credentials live in arduino_secrets.h, which is not committed.
// Copy arduino_secrets.example.h to arduino_secrets.h and fill in your own values.
#include "arduino_secrets.h"

#define BLYNK_PRINT Serial 

#include <Wire.h>
#include <ESP8266_Lib.h>
#include <BlynkSimpleShieldEsp8266.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "DHT.h"
#include <LiquidCrystal_I2C.h>

// --- 2. WIFI CREDENTIALS ---
char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = SECRET_WIFI_SSID;
char pass[] = SECRET_WIFI_PASS;

// --- 3. HARDWARE CONFIGURATION ---
#define EspSerial Serial1 
#define ESP8266_BAUD 115200

ESP8266 wifi(&EspSerial);
LiquidCrystal_I2C lcd(0x27, 16, 2); 

const int PIN_RED = 5;    
const int PIN_GREEN = 4; 

#define DHTPIN 7        
#define DHTTYPE DHT11  
DHT dht(DHTPIN, DHTTYPE);

MAX30105 particleSensor;

// --- 4. ALGORITHM VARIABLES ---
const byte RATE_SIZE = 6; // Balanced buffer size
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0; 
int beatAvg = 0;

// SpO2 Variables 
double avered = 0; double aveir = 0; 
double sumirrms = 0; double sumredrms = 0;
int i = 0; int Num = 30; 
double ESpO2 = 93.0; 
double FSpO2 = 0.7; double frate = 0.95; 

// Environment Vars
float humidity = 0.0;
float temperature = 0.0;

void setup() {
  Serial.begin(115200);            
  EspSerial.begin(ESP8266_BAUD);  
  
  delay(1000); 

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("System Init...");

  Serial.println("Connecting to Blynk...");
  Blynk.begin(auth, wifi, ssid, pass);
  
  lcd.setCursor(0,1); lcd.print("WiFi: OK");
  Serial.println("Blynk Connected!");
  delay(1000);
  
  dht.begin();
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  digitalWrite(PIN_RED, HIGH); 

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) { 
    Serial.println("MAX30105 not found.");
    lcd.clear(); lcd.print("Sensor Error");
    while (1); 
  }

  // --- SETTINGS (BALANCED MODE) ---
  Wire.setClock(400000); 
  
  // FIX 1: Prevent Saturation
  byte ledBrightness = 0x1F; // Default (31). Not too bright.
  int adcRange = 16384;      // Default range. Prevents "Blinding".
  
  // FIX 2: Keep High Speed (Solves the 40 vs 70 BPM issue)
  byte sampleAverage = 1;    // No averaging (Crisp signal)
  int sampleRate = 400;      // 400Hz (Fast scanning)
  
  byte ledMode = 2;       
  int pulseWidth = 411;      

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0); 

  lcd.clear();
  lcd.print("Place Finger...");
  Serial.println("System Ready. Balanced Mode Active.");
}

void loop() {
  if (Blynk.connected()) { Blynk.run(); }

  particleSensor.check(); 
  while (particleSensor.available()) {
    double fred = (double)particleSensor.getFIFORed();
    double fir = (double)particleSensor.getFIFOIR();

    i++;
    avered = avered * frate + fred * (1.0 - frate);
    aveir = aveir * frate + fir * (1.0 - frate); 
    sumredrms += (fred - avered) * (fred - avered); //υπολ, Μ.Ο 
    sumirrms += (fir - aveir) * (fir - aveir);//Υπολ ενεργειας παλμου

    if ((i % Num) == 0) {
      double R = (sqrt(sumredrms) / avered) / (sqrt(sumirrms) / aveir);
      double SpO2 = -23.3 * (R - 0.4) + 100 + 5.0; 
      ESpO2 = FSpO2 * ESpO2 + (1.0 - FSpO2) * SpO2;
      if (ESpO2 > 100) ESpO2 = 100;
      if (ESpO2 < 80) ESpO2 = 80; 
      sumredrms = 0.0; sumirrms = 0.0; i = 0;
    }

    long irValue = (long)fir;
    
    // FIX 3: Lower Threshold for detection
    // Since we went back to adcRange 16384, the raw numbers are smaller.
    // We lower the threshold to 7000 to ensure detection.
    if (irValue > 7000) { 
        if (checkForBeat(irValue) == true) {
            long delta = millis() - lastBeat;
            lastBeat = millis();
            
            float rawBPM = 60 / (delta / 1000.0);
            
            if (rawBPM < 255 && rawBPM > 20) {
                rates[rateSpot++] = (byte)rawBPM;
                rateSpot %= RATE_SIZE;
                
                beatAvg = 0;
                for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
                beatAvg /= RATE_SIZE;
            }
        }
    } else {
       beatAvg = 0; ESpO2 = 93.0;
       for(byte k=0; k<RATE_SIZE; k++) rates[k]=0;
    }
    particleSensor.nextSample(); 
  }

  static unsigned long lastScreenUpdate = 0;
  static unsigned long lastBlynkUpdate = 0;
  unsigned long currentMillis = millis();

  // DISPLAY (1.0s)
  if (currentMillis - lastScreenUpdate > 1000) {
     lastScreenUpdate = currentMillis;
     
     float newH = dht.readHumidity();
     float newT = dht.readTemperature();
     if (!isnan(newH)) humidity = newH;
     if (!isnan(newT)) temperature = newT;

     if (beatAvg > 40) {
        Serial.print("BPM: "); Serial.print(beatAvg);
        Serial.print(" | SpO2: "); Serial.println((int)ESpO2);
     }

     lcd.setCursor(0, 0);
     if (beatAvg > 40) {
        lcd.print("BPM:"); lcd.print(beatAvg); 
        lcd.print(" O2:"); lcd.print((int)ESpO2); lcd.print("%  "); 
     } else {
        lcd.print("Place Finger... ");
     }
     
     lcd.setCursor(0, 1);
     lcd.print("T:"); lcd.print((int)temperature); lcd.print("C ");
     lcd.print("H:"); lcd.print((int)humidity); lcd.print("%   ");

     bool fingerDetected = (beatAvg > 0);
     bool vitalStatsGood = (beatAvg >= 50 && beatAvg <= 110 && ESpO2 >= 90);

     if (!fingerDetected) {
        digitalWrite(PIN_RED, HIGH); digitalWrite(PIN_GREEN, LOW); 
     } 
     else if (vitalStatsGood) {
        digitalWrite(PIN_RED, LOW); digitalWrite(PIN_GREEN, HIGH); 
     } 
     else {
        digitalWrite(PIN_RED, HIGH); digitalWrite(PIN_GREEN, LOW); 
     }
  }

  // BLYNK (4.0s)
  if (currentMillis - lastBlynkUpdate > 4000) { 
     lastBlynkUpdate = currentMillis;

     if (Blynk.connected()) {
         Blynk.virtualWrite(V2, temperature);
         Blynk.virtualWrite(V3, humidity);
         
         if (beatAvg > 40) {
             Blynk.virtualWrite(V0, beatAvg);    
             Blynk.virtualWrite(V1, (int)ESpO2); 
             if (beatAvg > 110 || ESpO2 < 90) {
                 Blynk.logEvent("health_alert", "Abnormal Vitals!");
             }
         }
     }
  }
}