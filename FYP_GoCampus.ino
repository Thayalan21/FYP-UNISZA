#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <time.h>

// ================= WIFI & FIREBASE =================
#define WIFI_SSID     "Varisudevan"
#define WIFI_PASSWORD "thayalan"
#define API_KEY       "AIzaSyAap10fIsXqvyJVXGYEEwWDPJCIB8UXJPE"
#define DATABASE_URL  "gocampus-181a7-default-rtdb.asia-southeast1.firebasedatabase.app"

FirebaseData fbdo;
FirebaseData streamData;
FirebaseAuth auth;
FirebaseConfig config;

// ================= GPS =================
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD   9600

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

float currentLat  = 0.0;
float currentLng  = 0.0;
bool  gpsFix      = false;

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= IR SENSORS =================
const int sensorA = 14;
const int sensorB = 27;

// ================= PASSENGER LOGIC =================
int passengerCount = 0;
const int busCapacity = 40;

// State machine
int  state            = 0;
unsigned long lastDetectionTime  = 0;
unsigned long sequenceStartTime  = 0;

// How long sensor must stay LOW to count as real
// Increase this if getting false triggers
// Decrease this if missing fast movements
#define CONFIRM_MS       30    // sensor must be LOW for 30ms
#define COOLDOWN_MS     500    // wait 500ms after each count
#define SEQ_TIMEOUT_MS 3000   // reset if sequence takes >3s

// Confirmed trigger tracking
bool aConfirmed = false;
bool bConfirmed = false;
unsigned long aLowStart = 0;
unsigned long bLowStart = 0;

// ================= TIMING =================
unsigned long lastFirebaseUpdate = 0;
const unsigned long FIREBASE_INTERVAL_MS = 3000;
unsigned long lastScreenChange = 0;
const unsigned long LCD_INTERVAL_MS = 3000;
int screen = 0;

// ==========================================
// STREAM CALLBACK
// ==========================================
void streamCallback(FirebaseStream data) {
  String path = data.dataPath();

  if (path == "/passengerCount") {
    int cloudCount = data.intData();
    if (cloudCount == 0) {
      passengerCount = 0;
      state = 0;
      aConfirmed = false;
      bConfirmed = false;
      Serial.println(">>> [SYSTEM] RESET by App.");
    }
  }

  if (path == "/status") {
    String status = data.stringData();
    if (status == "INACTIVE") {
      passengerCount = 0;
      state = 0;
      aConfirmed = false;
      bConfirmed = false;
      Serial.println(">>> [SYSTEM] STANDBY mode.");
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, resuming...");
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // INPUT_PULLUP — stable HIGH when beam is clear
  // Goes LOW only when beam is broken
  pinMode(sensorA, INPUT_PULLUP);
  pinMode(sensorB, INPUT_PULLUP);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS initialized on UART2");

  lcd.begin();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  lcd.clear();
  lcd.print("WiFi Connected");
  delay(1000);

  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 100000 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  if (now < 100000) {
    Serial.println("\nTime sync FAILED — continuing anyway");
  } else {
    Serial.println("\nTime synced!");
  }

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);

  if (!Firebase.RTDB.beginStream(&streamData, "/GoCampus/Bus1")) {
    Serial.println("Stream Error: " + streamData.errorReason());
  }
  Firebase.RTDB.setStreamCallback(
    &streamData, streamCallback, streamTimeoutCallback);

  lcd.clear();
  lcd.print("System Ready");
  delay(1500);

  Serial.println("========================");
  Serial.println("  GO CAMPUS SYSTEM ON   ");
  Serial.println("========================");
  Serial.println("Sensor A = PIN 14 (ENTRY first)");
  Serial.println("Sensor B = PIN 27 (EXIT first)");
  Serial.println("========================");
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  updateGPS();
  handlePassengerCount();
  sendToFirebase();
  autoScrollLCD();
}

// ==========================================
// GPS UPDATE
// ==========================================
void updateGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  if (gps.location.isValid() && gps.location.isUpdated()) {
    currentLat = gps.location.lat();
    currentLng = gps.location.lng();
    gpsFix     = true;
  }

  static unsigned long lastGPSPrint = 0;
  if (millis() - lastGPSPrint > 5000) {
    lastGPSPrint = millis();
    Serial.println("------- GPS STATUS -------");
    Serial.print("Satellites: ");
    Serial.println(gps.satellites.isValid() ?
      String(gps.satellites.value()) : "Searching...");
    Serial.print("Location:   ");
    if (gpsFix) {
      Serial.print("LAT = ");
      Serial.print(currentLat, 6);
      Serial.print("  LNG = ");
      Serial.println(currentLng, 6);
    } else {
      Serial.println("No fix yet — go outside!");
    }
    Serial.print("Characters: ");
    Serial.println(gps.charsProcessed());
    Serial.println("--------------------------");
  }
}

// ==========================================
// CONFIRMED SENSOR READ
// Sensor must stay LOW for CONFIRM_MS
// before it counts as a real trigger
// This eliminates ALL noise/false triggers
// ==========================================
bool isConfirmed(int pin, unsigned long &lowStart, bool &confirmed) {
  int val = digitalRead(pin);

  if (val == LOW) {
    if (lowStart == 0) {
      // Just went LOW — start timer
      lowStart = millis();
    }
    if (!confirmed && (millis() - lowStart >= CONFIRM_MS)) {
      // Stayed LOW long enough — confirmed real trigger
      confirmed = true;
      return true;  // returns true ONCE when first confirmed
    }
  } else {
    // Sensor is HIGH — reset timer and confirmed flag
    lowStart  = 0;
    confirmed = false;
  }
  return false;
}

// ==========================================
// ENTRY/EXIT STATE MACHINE
// ENTRY: A confirmed LOW first → B confirmed LOW
// EXIT:  B confirmed LOW first → A confirmed LOW
// ==========================================
void handlePassengerCount() {

  // Read both sensors with confirmation
  bool trigA = isConfirmed(sensorA, aLowStart, aConfirmed);
  bool trigB = isConfirmed(sensorB, bLowStart, bConfirmed);

  // Raw readings for cooldown check
  int rawA = digitalRead(sensorA);
  int rawB = digitalRead(sensorB);

  switch (state) {

    // ---- IDLE ----
    case 0:
      // Skip if still in cooldown
      if (millis() - lastDetectionTime < COOLDOWN_MS) break;

      if (trigA) {
        // A confirmed first → start ENTRY sequence
        state = 1;
        sequenceStartTime = millis();
        Serial.println("[A] Entry sequence started");
      } else if (trigB) {
        // B confirmed first → start EXIT sequence
        state = 2;
        sequenceStartTime = millis();
        Serial.println("[B] Exit sequence started");
      }
      break;

    // ---- ENTRY SEQUENCE: waiting for B ----
    case 1:
      // Timeout — took too long, reset
      if (millis() - sequenceStartTime > SEQ_TIMEOUT_MS) {
        Serial.println("[TIMEOUT] Entry reset");
        state     = 0;
        aConfirmed = false;
        bConfirmed = false;
        aLowStart  = 0;
        bLowStart  = 0;
        break;
      }

      if (trigB) {
        // B confirmed — ENTRY complete
        if (passengerCount < busCapacity) {
          passengerCount++;
          Serial.print("✅ ENTRY → Count: ");
          Serial.println(passengerCount);
        } else {
          Serial.println("⚠️ BUS FULL");
        }
        lastDetectionTime = millis();
        state     = 3;
        aConfirmed = false;
        bConfirmed = false;
        aLowStart  = 0;
        bLowStart  = 0;
      }
      break;

    // ---- EXIT SEQUENCE: waiting for A ----
    case 2:
      // Timeout — took too long, reset
      if (millis() - sequenceStartTime > SEQ_TIMEOUT_MS) {
        Serial.println("[TIMEOUT] Exit reset");
        state     = 0;
        aConfirmed = false;
        bConfirmed = false;
        aLowStart  = 0;
        bLowStart  = 0;
        break;
      }

      if (trigA) {
        // A confirmed — EXIT complete
        if (passengerCount > 0) {
          passengerCount--;
          Serial.print("✅ EXIT → Count: ");
          Serial.println(passengerCount);
        } else {
          Serial.println("⚠️ Already at 0");
        }
        lastDetectionTime = millis();
        state     = 3;
        aConfirmed = false;
        bConfirmed = false;
        aLowStart  = 0;
        bLowStart  = 0;
      }
      break;

    // ---- COOLDOWN: wait for both to clear ----
    case 3:
      if (millis() - lastDetectionTime > COOLDOWN_MS) {
        if (rawA == HIGH && rawB == HIGH) {
          state = 0;
          Serial.println("[READY]");
        }
      }
      break;
  }
}

// ==========================================
// FIREBASE SEND
// ==========================================
void sendToFirebase() {
  if (!Firebase.ready()) return;
  if (millis() - lastFirebaseUpdate < FIREBASE_INTERVAL_MS) return;
  lastFirebaseUpdate = millis();

  String statusMsg;
  if (passengerCount >= busCapacity)
    statusMsg = "FULL";
  else if (passengerCount > 30)
    statusMsg = "ALMOST FULL";
  else
    statusMsg = "AVAILABLE";

  time_t now = time(nullptr);
  int ts = (now > 100000) ? (int)now : (int)(millis() / 1000);

  bool ok = true;

  ok &= Firebase.RTDB.setInt(
    &fbdo, "/GoCampus/Bus1/passengerCount", passengerCount);
  if (!ok) {
    Serial.println("passengerCount FAIL: " + fbdo.errorReason());
    return;
  }

  ok &= Firebase.RTDB.setInt(
    &fbdo, "/GoCampus/Bus1/capacity", busCapacity);
  if (!ok) {
    Serial.println("capacity FAIL: " + fbdo.errorReason());
    return;
  }

  ok &= Firebase.RTDB.setString(
    &fbdo, "/GoCampus/Bus1/status", statusMsg);
  if (!ok) {
    Serial.println("status FAIL: " + fbdo.errorReason());
    return;
  }

  ok &= Firebase.RTDB.setFloat(
    &fbdo, "/GoCampus/Bus1/latitude", currentLat);
  ok &= Firebase.RTDB.setFloat(
    &fbdo, "/GoCampus/Bus1/longitude", currentLng);
  if (!ok) {
    Serial.println("GPS FAIL: " + fbdo.errorReason());
    return;
  }

  ok &= Firebase.RTDB.setBool(
    &fbdo, "/GoCampus/Bus1/gpsFix", gpsFix);
  if (!ok) {
    Serial.println("gpsFix FAIL: " + fbdo.errorReason());
    return;
  }

  ok &= Firebase.RTDB.setInt(
    &fbdo, "/GoCampus/Bus1/timestamp", ts);
  if (!ok) {
    Serial.println("timestamp FAIL: " + fbdo.errorReason());
    return;
  }

  Serial.println("✅ Firebase OK — " + statusMsg +
    " | Count: " + String(passengerCount) + "/" + String(busCapacity) +
    " | GPS: " + (gpsFix ? "FIX (" + String(currentLat, 4)
                          + "," + String(currentLng, 4) + ")" : "NO FIX") +
    " | TS: " + String(ts));
}

// ==========================================
// LCD AUTO SCROLL — 3 screens
// ==========================================
void autoScrollLCD() {
  if (millis() - lastScreenChange < LCD_INTERVAL_MS) return;
  lastScreenChange = millis();
  screen = (screen + 1) % 3;
  lcd.clear();
  lcd.setCursor(0, 0);

  if (screen == 0) {
    lcd.print("GO CAMPUS BUS");
    lcd.setCursor(0, 1);
    lcd.print("Load:");
    lcd.print(passengerCount);
    lcd.print("/");
    lcd.print(busCapacity);

  } else if (screen == 1) {
    lcd.print("Bus Status:");
    lcd.setCursor(0, 1);
    if (passengerCount >= busCapacity)
      lcd.print("BUS FULL");
    else if (passengerCount > 30)
      lcd.print("Almost Full");
    else
      lcd.print("Seats Free");

  } else {
    lcd.print("GPS:");
    lcd.print(gpsFix ? "FIX" : "Searching...");
    lcd.setCursor(0, 1);
    if (gpsFix) {
      lcd.print(String(currentLat, 2));
      lcd.print(",");
      lcd.print(String(currentLng, 2));
    } else {
      lcd.print("No signal yet");
    }
  }
}