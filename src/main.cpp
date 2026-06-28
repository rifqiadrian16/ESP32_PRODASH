#include <Arduino.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Update.h> // Menggantikan HTTPUpdate.h untuk Push OTA

// =======================================================
#define FIRMWARE_VERSION "1.1.0"  // Versi firmware ini (ganti setiap rilis)

WebServer server(80); // Server untuk menerima file OTA

// =======================================================
// 1. PENGATURAN PIN & VARIABEL GLOBAL
// =======================================================
const int RELAY_PIN = 4;
const int SENSOR_UNLOCK_PIN = 18;
float currentO2Volt = 0.0;

// Variabel OBD WiFi Dinamis (Bisa diubah lewat aplikasi)
WiFiClient obdClient;
String current_obd_ssid = "WiFi_OBDII";
String current_obd_pass = "";
String current_obd_ip   = "192.168.0.10";
int current_obd_port    = 35000;
String current_obd_pin  = "1234";

Preferences preferences;
int currentMode = 1;

BluetoothSerial SerialBT;
// Variabel MAC OBD Dinamis
uint8_t targetMac[6]; 
bool hasStoredMac = false;
bool triggerBtScan = false;
bool triggerWifiScan = false;
bool waitingMacNotified = false;
String pendingRawCommand = "";

struct FoundDevice { String name; String mac; bool ready = false; };
FoundDevice foundQueue[10];
int foundCount = 0;

// Variabel Data Mobil
int currentSpeed = 0, currentRPM = 0, currentTemp = 0;
bool isLocked = false, obdConnected = false;
float currentVolt = 0.0, currentMAF = 0.0;
int currentIAT = 0, currentTiming = 0, currentThrottle = 0;
float currentSTFT = 0.0, currentLTFT = 0.0;

// Variabel Kontrol
unsigned long lastValidDataTime = 0;
unsigned long lastReconnectAttempt = 0;
int reconnectFailCount = 0;
unsigned long lastUnlockTime = 0;
unsigned long relayTriggerTime = 0;
bool isRelayActive = false;

bool isBtScanning = false;
unsigned long btScanStartTime = 0;
const int BT_SCAN_DURATION = 5000; 

bool activeTestRunning = false;
int activeTestCylinder = 0; 
unsigned long lastActiveTestSend = 0;
const unsigned long ACTIVE_TEST_INTERVAL = 300; 

unsigned long lastActiveTestHeartbeat = 0;       
const unsigned long ACTIVE_TEST_HEARTBEAT_TIMEOUT = 1500; 

unsigned long activeTestStartTime = 0;           
const unsigned long ACTIVE_TEST_MAX_DURATION = 6000; 

bool o2StreamRunning = false;
unsigned long lastO2StreamSend = 0;
const unsigned long O2_STREAM_INTERVAL = 400;

bool autoLockEnabled = true;
int autoLockSpeed = 15; 

const int POLL_SEQ[] = {
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 2, 9,  // RPM -> Throttle -> Suhu  -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 3, 9,  // RPM -> Throttle -> MAF   -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 4, 9,  // RPM -> Throttle -> Volt  -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 5, 9,  // RPM -> Throttle -> IAT   -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 6, 9,  // RPM -> Throttle -> STFT  -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 7, 9,  // RPM -> Throttle -> LTFT  -> Throttle
  0, 9, 1, 9,  // RPM -> Throttle -> Speed -> Throttle
  0, 9, 8, 9   // RPM -> Throttle -> Timing-> Throttle
};
const int POLL_SEQ_LEN = 56;
int seqIdx = 0;

// =======================================================
// 2. PENGATURAN BLE (Pipa Komunikasi ke HP)
// =======================================================
String sendOBD(String command);
String sendOBDWiFi(String command);
void requestStopActiveTest(String reason);

BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic = NULL;
BLECharacteristic* pRxCharacteristic = NULL;
bool deviceConnected = false;

bool manualDisconnect = false;

#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_TX "beb5483e-36e1-4688-b7f5-ea07361b26a8" 
#define CHARACTERISTIC_UUID_RX "8c38148b-3db4-46c6-bb50-51b68181fb6b" 

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("[BLE] HP Terhubung!");
    };
     void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("[BLE] HP Terputus! Memulai ulang pancaran...");

      if (activeTestRunning) {
        Serial.println("🛑 [SAFETY] BLE Disconnect saat Active Test jalan! Auto-stop dipaksa.");
        requestStopActiveTest("BLE HP disconnect");
      }

      if (o2StreamRunning) {                                  
        o2StreamRunning = false;                              
        Serial.println("🛑 [SAFETY] BLE Disconnect saat O2 Stream jalan! Auto-stop dipaksa.");
      }                                                       

      pServer->startAdvertising(); 
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue().c_str();
      Serial.println("[BLE] Perintah dari HP: " + rxValue);

      if (rxValue.startsWith("SET_OBD_MAC:")) {
        String macStr = rxValue.substring(12); 
        preferences.putString("obd_mac", macStr);
        Serial.println("MAC OBD Baru Disimpan: " + macStr);
        
        for (int i = 0; i < 6; i++) {
          targetMac[i] = strtol(macStr.substring(i * 3, i * 3 + 2).c_str(), NULL, 16);
        }
        hasStoredMac = true;
      }

      else if (rxValue.startsWith("SET_OBD_PIN:")) {
        current_obd_pin = rxValue.substring(12);
        current_obd_pin.trim();
        preferences.putString("obd_pin", current_obd_pin);
        Serial.println("[BLE] PIN OBD2 Disimpan: " + current_obd_pin);
      }

      else if (rxValue.startsWith("WIFI_SSID:")) {
        String newSSID = rxValue.substring(10);
        preferences.putString("ssid", newSSID);
        Serial.println("SSID Customer Disimpan: " + newSSID);
      } 
      else if (rxValue.startsWith("WIFI_PASS:")) {
        String newPass = rxValue.substring(10);
        preferences.putString("pass", newPass);
        Serial.println("Password Customer Disimpan.");
      }

      else if (rxValue.startsWith("SET_OBD_WIFI_SSID:")) {
        String ssidWifi = rxValue.substring(18);
        preferences.putString("obd_ssid", ssidWifi);
        Serial.println("SSID OBD WiFi Disimpan: " + ssidWifi);
      }
      else if (rxValue.startsWith("SET_OBD_WIFI_IP:")) {
        String ipWifi = rxValue.substring(16);
        preferences.putString("obd_ip", ipWifi);
        Serial.println("IP OBD WiFi Disimpan: " + ipWifi);
      }
      else if (rxValue.startsWith("SET_OBD_WIFI_PORT:")) {
        int portWifi = rxValue.substring(18).toInt();
        preferences.putInt("obd_port", portWifi);
        Serial.println("Port OBD WiFi Disimpan: " + String(portWifi));
      }

      else if (rxValue == "DISCONNECT_OBD") {
        manualDisconnect = true;
        SerialBT.disconnect();
        if (obdClient.connected()) obdClient.stop();
        Serial.println("Status: Standby/Bypass (Akses Car Scanner diijinkan).");
      } 
      else if (rxValue == "CONNECT_OBD") {
        manualDisconnect = false;
        lastReconnectAttempt = 0;
        Serial.println("Status: Resuming OBD Connection...");
      }

      else if (rxValue == "START_BT_SCAN") {
        triggerBtScan = true;
        Serial.println("Perintah Radar Scan BT Diterima!");
      }

      else if (rxValue == "START_WIFI_SCAN") {
        triggerWifiScan = true;
        Serial.println("Perintah Radar Scan WiFi Diterima!");
      }

      else if (rxValue == "SET_MODE_0") {
        preferences.putInt("mode", 0);
        Serial.println("Mode berubah ke 0 (OTA). Restarting...");
        delay(500); ESP.restart();
      } 
      else if (rxValue == "SET_MODE_1") {
        preferences.putInt("mode", 1);
        preferences.putInt("last_mode", 1);
        Serial.println("Mode berubah ke 1 (BT). Restarting...");
        delay(500); ESP.restart();
      } 
      else if (rxValue == "SET_MODE_2") {
        preferences.putInt("mode", 2);
        preferences.putInt("last_mode", 2);
        Serial.println("Mode berubah ke 2 (WiFi). Restarting...");
        delay(500); ESP.restart();
      }
      else if (rxValue == "AUTOLOCK_ON") {
        autoLockEnabled = true;
        preferences.putBool("autolock", true);
        Serial.println("Auto Lock: ENABLED");
      }
      else if (rxValue == "AUTOLOCK_OFF") {
        autoLockEnabled = false;
        preferences.putBool("autolock", false);
        Serial.println("Auto Lock: DISABLED");
      }
      else if (rxValue.startsWith("SET_LOCK_SPEED:")) {
        autoLockSpeed = rxValue.substring(15).toInt();
        preferences.putInt("lockspeed", autoLockSpeed);
        Serial.println("Auto Lock Speed: " + String(autoLockSpeed) + " km/h");
      }
      else if (rxValue == "GET_CONFIG") {
        if (deviceConnected) {
          String cfg = "CONFIG:autolock=" + String(autoLockEnabled ? 1 : 0) +
                      ",lockspeed=" + String(autoLockSpeed) +
                      ",mode=" + String(currentMode) +
                      ",hasmac=" + String(hasStoredMac ? 1 : 0);
          pTxCharacteristic->setValue(cfg.c_str());
          pTxCharacteristic->notify();
          Serial.println("[BLE] Config dikirim ke HP: " + cfg);
        }
      }
      else if (rxValue == "FACTORY_RESET") {
        Serial.println("⚠️ [BLE] Perintah Factory Reset Diterima! Menghapus semua memori...");
        preferences.clear();
        if (deviceConnected) {
          pTxCharacteristic->setValue("STATUS:RESET_DONE");
          pTxCharacteristic->notify();
        }
        delay(500); 
        ESP.restart();
      }
      else if (rxValue == "TEST_RELAY") {
        static bool statusRelayTest = false; 
        statusRelayTest = !statusRelayTest; 
        if (statusRelayTest) {
            digitalWrite(RELAY_PIN, HIGH); 
            Serial.println("🔥 TEST: RELAY ON (Cetek!)");
        } else {
            digitalWrite(RELAY_PIN, LOW);  
            Serial.println("💤 TEST: RELAY OFF");
        }
      }
      else if (rxValue.startsWith("ACTIVE_TEST_CYL:")) {
        int cyl = rxValue.substring(16).toInt();
        if (cyl >= 1 && cyl <= 4) {
          unsigned long now = millis();
          if (!activeTestRunning || activeTestCylinder != cyl) {
            activeTestStartTime = now;
            lastActiveTestSend = 0; 
          }
          o2StreamRunning = false;
          activeTestCylinder = cyl;
          activeTestRunning = true;
          lastActiveTestHeartbeat = now; 
          Serial.println("🧪 ACTIVE TEST: Heartbeat Cylinder Cut #" + String(cyl));
        } else {
          Serial.println("⚠️ ACTIVE TEST: Nomor cylinder tidak valid: " + String(cyl));
        }
      }
      else if (rxValue == "ACTIVE_TEST_STOP") {
        requestStopActiveTest("Perintah STOP manual dari HP");
      }
      else if (rxValue.startsWith("RAW:")) {
        pendingRawCommand = rxValue.substring(4); 
        Serial.print("[TERMINAL] Pesan dititipkan ke Antrean VIP: "); 
        Serial.println(pendingRawCommand);
      }
      else if (rxValue == "O2_STREAM_START") {
        if (!activeTestRunning) {
          o2StreamRunning = true;
          lastO2StreamSend = 0; // paksa kirim PID di iterasi loop() berikutnya
          Serial.println("🧪 O2 STREAM: Mode streaming AKTIF.");
        } else {
          Serial.println("⚠️ O2 STREAM: Ditolak, Active Test sedang berjalan.");
        }
      }
      else if (rxValue == "O2_STREAM_STOP") {
        o2StreamRunning = false;
        Serial.println("🧪 O2 STREAM: Mode streaming DIHENTIKAN.");
      }
    }
};

void initBLE() {
  BLEDevice::init("LivinaProDash");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  pRxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_RX,
                        BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  BLEAdvertisementData advData;
  advData.setName("LivinaProDash");
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();

  Serial.println("[BLE] Sinyal LivinaProDash sudah memancar!");
}

void kirimDataKeHP() {
  if (deviceConnected) {
    String dataString = "{\"s\":" + String(currentSpeed) + 
                        ",\"r\":" + String(currentRPM) + 
                        ",\"t\":" + String(currentTemp) + 
                        ",\"v\":" + String(currentVolt, 1) + 
                        ",\"m\":" + String(currentMAF, 2) + 
                        ",\"i\":" + String(currentIAT) + 
                        ",\"st\":" + String(currentSTFT, 1) + 
                        ",\"lt\":" + String(currentLTFT, 1) + 
                        ",\"tm\":" + String(currentTiming) + 
                        ",\"th\":" + String(currentThrottle) + 
                        ",\"l\":" + String(isLocked ? 1 : 0) + "}";
    
    pTxCharacteristic->setValue(dataString.c_str());
    pTxCharacteristic->notify(); 
  }
}

// =======================================================
// 3. FUNGSI KOMUNIKASI OBD
// =======================================================
String sendOBD(String command) {
  Serial.println("[OBD_BT_TX] MENGIRIM: " + command);
  while (SerialBT.available()) SerialBT.read();
  SerialBT.print(command + "\r");
  String response = "";
  response.reserve(50);
  unsigned long startTimer = millis();
  while (millis() - startTimer < 800) {
    if (SerialBT.available()) {
      char c = SerialBT.read();
      if (c == '>') break;
      response += c;
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  response.replace("\r", ""); response.replace("\n", ""); response.replace(" ", "");
  Serial.println("[OBD_BT_RX] DITERIMA: " + response);
  return response;
}

String sendOBDWiFi(String command) {
  if (!obdClient.connected()) return "";
  Serial.println("[OBD_WIFI_TX] MENGIRIM: " + command);
  while (obdClient.available()) obdClient.read(); 
  obdClient.print(command + "\r");
  String response = "";
  response.reserve(50);
  unsigned long startTimer = millis();
  while (millis() - startTimer < 800) {
    if (obdClient.available()) {
      char c = obdClient.read();
      if (c == '>') break;
      response += c;
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  response.replace("\r", ""); response.replace("\n", ""); response.replace(" ", "");
  Serial.println("[OBD_WIFI_RX] DITERIMA: " + response);
  return response;
}

bool pendingActiveTestStopSend = false; 

void requestStopActiveTest(String reason) {
  if (!activeTestRunning && activeTestCylinder == 0 && !pendingActiveTestStopSend) {
    return; 
  }
  activeTestRunning = false;
  activeTestCylinder = 0;
  pendingActiveTestStopSend = true; 
  Serial.println("🛑 ACTIVE TEST: Permintaan stop dicatat [" + reason + "], menunggu eksekusi...");
}

void initOBD2WiFi() {
  Serial.println("=== MULAI INISIALISASI ECU (WIFI) ===");
  sendOBDWiFi("ATZ"); delay(500); 
  sendOBDWiFi("ATE0"); delay(200); 
  sendOBDWiFi("ATSP5"); delay(200);
  sendOBDWiFi("ATSH 81 10 FC"); delay(200); 
  sendOBDWiFi("ATAT1"); delay(100); 
  sendOBDWiFi("ATST 10"); delay(100); 
  sendOBDWiFi("ATFI"); delay(1500);
  Serial.println("=== INISIALISASI ECU SELESAI ===");
}

void initOBD2() {
  Serial.println("=== MULAI INISIALISASI ECU (BLUETOOTH) ===");
  sendOBD("ATZ"); delay(500); 
  sendOBD("ATE0"); delay(200); 
  sendOBD("ATSP5"); delay(200);
  sendOBD("ATSH 81 10 FC"); delay(200); 
  sendOBD("ATAT1"); delay(100); 
  sendOBD("ATST 10"); delay(100); 
  sendOBD("ATFI"); delay(1500);
  Serial.println("=== INISIALISASI ECU SELESAI ===");
}

// =======================================================
// 4. SETUP & LOOP
// =======================================================
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW);
  pinMode(SENSOR_UNLOCK_PIN, INPUT_PULLUP);

  preferences.begin("LivinaDash", false);

  currentMode = preferences.getInt("mode", 1);
  current_obd_pin = preferences.getString("obd_pin", "1234");
  String savedSSID = preferences.getString("ssid", "Iqi"); 
  String savedPass = preferences.getString("pass", "12345678");

  current_obd_ssid = preferences.getString("obd_ssid", "WiFi_OBDII");
  current_obd_pass = preferences.getString("obd_pass", "");
  current_obd_ip   = preferences.getString("obd_ip", "192.168.0.10");
  current_obd_port = preferences.getInt("obd_port", 35000);

  String storedMac = preferences.getString("obd_mac", "");
  if (storedMac != "" && storedMac.length() >= 17) {
    for (int i = 0; i < 6; i++) {
      targetMac[i] = strtol(storedMac.substring(i * 3, i * 3 + 2).c_str(), NULL, 16);
    }
    hasStoredMac = true;
  }

  autoLockEnabled = preferences.getBool("autolock", true);
  autoLockSpeed = preferences.getInt("lockspeed", 15);

  Serial.println("\n--- LIVINA PRO DASH V2 (MASS READY) ---");
  if (currentMode != 0) {
    initBLE(); 
  }

  if (currentMode == 0) {
    // ==========================================
    // MODE 0: PUSH OTA (UPDATE DARI LAPTOP)
    // ESP32 membuka webserver di port 80 dan
    // menunggu LivinaUpdater.exe mengirim firmware
    // ==========================================
    Serial.println("MASUK MODE OTA (PUSH DARI LAPTOP)...");
    Serial.println("Firmware saat ini: v" FIRMWARE_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    unsigned long wifiStart = millis();
    Serial.print("[WIFI] Menghubungkan ke hotspot");
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - wifiStart > 20000) {
        Serial.println("\n[WIFI] Gagal konek! Balik ke Mode 1...");
        preferences.putInt("mode", 1);
        delay(500); ESP.restart();
      }
      delay(500); Serial.print(".");
    }
    Serial.println("\n[WIFI] Terhubung! IP: " + WiFi.localIP().toString());
    Serial.println("Silakan masukkan IP di atas ke aplikasi LivinaUpdater.exe");

    // Lapor status ke ThingSpeak (Tetap dipertahankan sesuai aslinya)
    {
      HTTPClient http;
      String url = "http://api.thingspeak.com/update?api_key=D3HJLFYFGO4PLG08&field1=" + WiFi.localIP().toString();
      http.begin(url); http.GET(); http.end();
      Serial.println("[THINGSPEAK] IP dilaporkan.");
    }

    // --- SETUP WEBSERVER UNTUK TERIMA FIRMWARE ---
    server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "GAGAL" : "OK");
      delay(1000);
      ESP.restart(); // Otomatis restart setelah file diterima
    }, []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Menerima File: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("[OTA] Update Sukses! Total Ukuran: %u Bytes\n", upload.totalSize);
          // Kembalikan ke mode operasional setelah sukses
          preferences.putInt("mode", preferences.getInt("last_mode", 1)); 
        } else {
          Update.printError(Serial);
        }
      }
    });

    server.begin();
    Serial.println("Server OTA Port 80 berjalan... Menunggu file dari laptop.");

  } 
  else if (currentMode == 1) {
    WiFi.mode(WIFI_OFF);
    Serial.println("Mencari OBD2...");
    SerialBT.begin("LivinaProDash_OBD", true);
    SerialBT.setPin(current_obd_pin.c_str());
  } 
  else if (currentMode == 2) {
    Serial.println("-> MODE 2: OBD WIFI AKTIF");
    WiFi.mode(WIFI_STA);
    WiFi.begin(current_obd_ssid.c_str(), current_obd_pass.c_str());
  }
}

void loop() {
  if (currentMode == 0) {
    // =======================================================
    // KAMAR 1: MODE 0 (OTA PUSH)
    // Pantau terus paketan firmware yang masuk dari laptop
    // =======================================================
    server.handleClient();
    vTaskDelay(2 / portTICK_PERIOD_MS);
  } 
  else if (currentMode == 1) {
    // =======================================================
    // KAMAR 2: MODE 1 (OPERASIONAL HARIAN VIA OBD BLUETOOTH)
    // =======================================================
    if (pendingActiveTestStopSend) {
      pendingActiveTestStopSend = false;
      String res = sendOBD("300C0000");
      res.replace("\r", " ");
      res.replace("\n", "");

      if (deviceConnected) {
        String msg = "RAW_RES:ACTIVE_TEST_STOPPED=" + res;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
      }
      Serial.println("🛑 ACTIVE TEST: 300C0000 terkirim -> ECU balas: " + res);
      lastValidDataTime = millis();
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return; 
    }

    if (activeTestRunning) {
      unsigned long now = millis();
      if (now - lastActiveTestHeartbeat > ACTIVE_TEST_HEARTBEAT_TIMEOUT) {
        requestStopActiveTest("Heartbeat HP timeout");
      }
      else if (now - activeTestStartTime > ACTIVE_TEST_MAX_DURATION) {
        requestStopActiveTest("Hard-cap durasi maksimum tercapai");
      }
    }

    if (activeTestRunning) {
      if (millis() - lastActiveTestSend >= ACTIVE_TEST_INTERVAL) {
        lastActiveTestSend = millis();
        String pid = "300C0" + String(activeTestCylinder) + "00";
        String res = sendOBD(pid);
        res.replace("\r", " ");
        res.replace("\n", "");
        if (deviceConnected) {
          String msg = "RAW_RES:" + res;
          pTxCharacteristic->setValue(msg.c_str());
          pTxCharacteristic->notify();
        }
        lastValidDataTime = millis();
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return; 
    }

    if (o2StreamRunning) {                                            // ⬅️ BARU
      if (millis() - lastO2StreamSend >= O2_STREAM_INTERVAL) {
        lastO2StreamSend = millis();
        String res = sendOBD("2211180401");
        lastValidDataTime = millis();
        int idx = res.indexOf("621118");
        if (idx != -1 && res.length() >= idx + 8) {
          int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
          currentO2Volt = A * 0.01;
          if (deviceConnected) {
            String msg = "O2:" + String(currentO2Volt, 2);
            pTxCharacteristic->setValue(msg.c_str());
            pTxCharacteristic->notify();
          }
        }
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return; 
    }

    if (pendingRawCommand != "") {
      Serial.println("[TERMINAL VIP] Mengeksekusi perintah...");
      String rawRes = sendOBD(pendingRawCommand);
      rawRes.replace("\r", " ");
      rawRes.replace("\n", "");
      if (deviceConnected) {
        String msg = "RAW_RES:" + rawRes;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
      }
      Serial.println("[TERMINAL VIP] Balasan dikirim ke HP: " + rawRes);
      pendingRawCommand = ""; 
      vTaskDelay(200 / portTICK_PERIOD_MS); 
      return; 
    }
    
    if (triggerBtScan) {
      triggerBtScan = false;
      isBtScanning = true;
      btScanStartTime = millis();

      if (deviceConnected) {
        pTxCharacteristic->setValue("SCAN_STATUS:SCANNING");
        pTxCharacteristic->notify();
      }
      Serial.println("[BT] Memulai Async Radar...");
      foundCount = 0;
      for (int i = 0; i < 10; i++) {
        foundQueue[i].ready = false;
        foundQueue[i].name = "";
        foundQueue[i].mac = "";
      }

      SerialBT.discoverAsync([](BTAdvertisedDevice* pDevice) {
      String name = pDevice->getName().c_str();
      if (name.length() > 0 && foundCount < 10) {
        foundQueue[foundCount].name = name;
        foundQueue[foundCount].mac  = pDevice->getAddress().toString().c_str();
        foundQueue[foundCount].ready = true;
        foundCount++;
      }
     });
    }

    if (isBtScanning && millis() - btScanStartTime >= BT_SCAN_DURATION) {
      SerialBT.discoverAsyncStop();
      isBtScanning = false;
      if (deviceConnected) {
        pTxCharacteristic->setValue("SCAN_STATUS:DONE");
        pTxCharacteristic->notify();
      }
      Serial.println("[BT] Radar selesai.");
    }

    if (triggerWifiScan) {
      triggerWifiScan = false;
      if (deviceConnected) {
        pTxCharacteristic->setValue("SCAN_STATUS:SCANNING");
        pTxCharacteristic->notify();
      }
      Serial.println("[WIFI] Memulai Radar WiFi terdekat...");
      WiFi.mode(WIFI_STA); 
      WiFi.disconnect();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      
      int n = WiFi.scanNetworks();
      if (n > 0) {
        for (int i = 0; i < n; ++i) {
          String ssid = WiFi.SSID(i);
          if (ssid.length() > 0) {
            String msg = "SCAN_FOUND:" + ssid + "|WIFI";
            if (deviceConnected) {
              pTxCharacteristic->setValue(msg.c_str());
              pTxCharacteristic->notify();
              vTaskDelay(150 / portTICK_PERIOD_MS); 
            }
          }
        }
      }
      WiFi.mode(WIFI_OFF); 
      if (deviceConnected) {
        pTxCharacteristic->setValue("SCAN_STATUS:DONE");
        pTxCharacteristic->notify();
      }
      Serial.println("[WIFI] Radar Selesai.");
      return;
    }

    for (int i = 0; i < foundCount; i++) {
      if (foundQueue[i].ready && deviceConnected) {
        String msg = "SCAN_FOUND:" + foundQueue[i].name + "|" + foundQueue[i].mac;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
        foundQueue[i].ready = false;
        vTaskDelay(150 / portTICK_PERIOD_MS);
      }
    }

    if (manualDisconnect) {
      vTaskDelay(500 / portTICK_PERIOD_MS); 
      return; 
    }

    if (!SerialBT.connected(10)) {
      if (obdConnected) { obdConnected = false; isLocked = false; reconnectFailCount = 0; }

      if (!hasStoredMac) {
        if (deviceConnected) {
          static unsigned long lastMacNotify = 0;
          if (millis() - lastMacNotify > 2000) { 
            pTxCharacteristic->setValue("STATUS:WAITING_MAC");
            pTxCharacteristic->notify();
            lastMacNotify = millis();
          }
        }
        vTaskDelay(500 / portTICK_PERIOD_MS); 
        return;
      }

      waitingMacNotified = false;

      unsigned long interval = min(5000UL + (unsigned long)reconnectFailCount * 2000UL, 15000UL);
      if (millis() - lastReconnectAttempt > interval) {
        lastReconnectAttempt = millis();
        reconnectFailCount++;
        Serial.println("[OBD] Memutus koneksi lama...");
        SerialBT.disconnect(); vTaskDelay(200 / portTICK_PERIOD_MS);
        Serial.println("[OBD] Menghapus pairing nyangkut...");
        SerialBT.unpairDevice(targetMac); vTaskDelay(500 / portTICK_PERIOD_MS); 
        
        Serial.printf("[OBD] MENCOBA KONEK KE MAC: %02X:%02X:%02X:%02X:%02X:%02X dengan PIN: %s\n", 
                      targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5], current_obd_pin.c_str());
                      
        if (SerialBT.connect(targetMac)) {
          Serial.println("[OBD] KONEKSI FISIK BLUETOOTH BERHASIL! Menyiapkan komunikasi...");
          vTaskDelay(500 / portTICK_PERIOD_MS); SerialBT.print("\r"); vTaskDelay(500 / portTICK_PERIOD_MS);
          while (SerialBT.available()) SerialBT.read();
          reconnectFailCount = 0; 
          initOBD2(); 
          obdConnected = true; 
          lastValidDataTime = millis();
          Serial.println("[OBD] ESP32 & ECU SIAP TEMPUR!");
        } else {
          Serial.println("[OBD] GAGAL KONEK FISIK! ELM327 menolak atau tidak ditemukan.");
        }
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      return;
    }

    if (millis() - lastValidDataTime > 8000 && obdConnected) {
      SerialBT.disconnect(); obdConnected = false; vTaskDelay(500 / portTICK_PERIOD_MS); return;
    }

    if (pendingRawCommand != "") {
      Serial.println("[BYPASS BT] Menembak kode manual...");
      String res = sendOBD(pendingRawCommand);
      if (deviceConnected) {
        String msg = "RAW_OBD:" + res;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
      }
      pendingRawCommand = "";
      lastValidDataTime = millis();
      vTaskDelay(200 / portTICK_PERIOD_MS);
      return; 
    }

    int pid = POLL_SEQ[seqIdx];
    seqIdx = (seqIdx + 1) % POLL_SEQ_LEN;
    bool dataBerubah = false;

    if (pid == 0) { 
      String res = sendOBD("2212010401"); 
      int idx = res.indexOf("621201");
      if (idx != -1 && res.length() >= idx + 10) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        int B = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
        currentRPM = (int)((A * 3200.0) + (B * 12.5));
        lastValidDataTime = millis();
        dataBerubah = true;
      }
    }
    else if (pid == 1) { 
      String res = sendOBD("2211020401");
      int idx = res.indexOf("621102");
      if (idx != -1 && res.length() >= idx + 8) {
        currentSpeed = (int)(strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) * 2.0);
        lastValidDataTime = millis();
        dataBerubah = true;
        if (autoLockEnabled && currentSpeed >= autoLockSpeed && !isLocked && !isRelayActive) {
          digitalWrite(RELAY_PIN, HIGH); 
          relayTriggerTime = millis();  
          isRelayActive = true;         
          isLocked = true;
        }
      }
    }
    else if (pid == 2) { 
      String res = sendOBD("2211010401");
      int idx = res.indexOf("621101");
      if (idx != -1 && res.length() >= idx + 8) {
        currentTemp = (int)strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) - 50;
        dataBerubah = true;
      }
    }
    else if (pid == 3) { 
      String res = sendOBD("2212090401");
      int idx = res.indexOf("621209");
      if (idx != -1 && res.length() >= idx + 10) {
        float valA = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        float valB = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
        currentMAF = (valA * 1.28) + ((valB * 1.27) / 255.0);
        dataBerubah = true;
      }
    }
    else if (pid == 4) { 
      String res = sendOBD("ATRV");
      if (res.indexOf("V") != -1) { res.replace("V", ""); currentVolt = res.toFloat();
        dataBerubah = true;
      }
    }
    else if (pid == 5) { 
      String res = sendOBD("2211060401"); 
      int idx = res.indexOf("621106");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentIAT = A - 50; 
        dataBerubah = true;
      }
    }
    else if (pid == 6) { 
      String res = sendOBD("2211230401");
      int idx = res.indexOf("621123");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentSTFT = A - 100.0;
        dataBerubah = true;
      }
    }
    else if (pid == 7) { 
      String res = sendOBD("2211250401");
      int idx = res.indexOf("621125");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentLTFT = A - 100.0;
        dataBerubah = true;
      }
    }
    else if (pid == 8) { 
      String res = sendOBD("22110A0401");
      int idx = res.indexOf("62110A");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentTiming = 110 - A; 
        dataBerubah = true;
      }
    }
    else if (pid == 9) { 
      String res = sendOBD("22120E0401");
      res.replace(" ","");
      int idx = res.indexOf("62120E");
      if (idx != -1 && res.length() >= idx + 10) {
        int A = strtol(res.substring(6, 8).c_str(), NULL, 16); 
        int B = strtol(res.substring(8, 10).c_str(), NULL, 16);
        float volt = (A * 1.28) + (B * 1.27 / 255.0);
        float percent = ((volt - 0.78) / (4.48 - 0.78)) * 100.0;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        currentThrottle = (int)percent; 
        dataBerubah = true;
      }
    }

    if (dataBerubah) {
      kirimDataKeHP();
    }

    if (isRelayActive && (millis() - relayTriggerTime >= 500)) {
      digitalWrite(RELAY_PIN, LOW); isRelayActive = false;
    }

    if (digitalRead(SENSOR_UNLOCK_PIN) == LOW) {
      if (millis() - lastUnlockTime > 1000) {
        if (isLocked) { 
          isLocked = false; 
          kirimDataKeHP(); 
          Serial.println("[LOCK] Pintu dibuka manual.");
        }
        lastUnlockTime = millis();
      }
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  else if (currentMode == 2) {
    // =======================================================
    // KAMAR 3: MODE 2 (OPERASIONAL HARIAN VIA OBD WIFI)
    // =======================================================
    if (manualDisconnect) {
      if (obdClient.connected()) obdClient.stop();
      vTaskDelay(500 / portTICK_PERIOD_MS);
      return; 
    }

    if (WiFi.status() != WL_CONNECTED) {
      obdConnected = false;
      Serial.println("[WIFI] Mencari Hotspot OBD2...");
      static unsigned long lastWifiReconnect = 0;
      if (millis() - lastWifiReconnect > 5000) { 
        lastWifiReconnect = millis();
        WiFi.disconnect();
        vTaskDelay(100 / portTICK_PERIOD_MS);
        WiFi.begin(current_obd_ssid.c_str(), current_obd_pass.c_str());
        Serial.println("[WIFI] Mencoba reconnect...");
      }
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      return;
    }

    if (!obdClient.connected()) {
      if (obdConnected) { obdConnected = false; isLocked = false; reconnectFailCount = 0; }
      
      Serial.println("[TCP] Menyambungkan ke IP OBD2...");
      if (obdClient.connect(current_obd_ip.c_str(), current_obd_port)) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        obdClient.print("\r"); 
        vTaskDelay(500 / portTICK_PERIOD_MS);
        initOBD2WiFi();
        obdConnected = true;
        lastValidDataTime = millis();
        Serial.println("[TCP] Berhasil Konek ke OBD WiFi!");
      } else {
        Serial.println("[TCP] Gagal konek, coba lagi...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
      }
      return;
    }

    if (millis() - lastValidDataTime > 8000 && obdConnected) {
      obdClient.stop(); obdConnected = false; vTaskDelay(500 / portTICK_PERIOD_MS); return;
    }

    if (pendingActiveTestStopSend) {
      pendingActiveTestStopSend = false;
      String res = sendOBDWiFi("300C0000");
      res.replace("\r", " ");
      res.replace("\n", "");
      if (deviceConnected) {
        String msg = "RAW_RES:ACTIVE_TEST_STOPPED=" + res;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
      }
      Serial.println("🛑 ACTIVE TEST: 300C0000 terkirim (WiFi) -> ECU balas: " + res);
      lastValidDataTime = millis();
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return;
    }

    if (activeTestRunning) {
      unsigned long now = millis();
      if (now - lastActiveTestHeartbeat > ACTIVE_TEST_HEARTBEAT_TIMEOUT) {
        requestStopActiveTest("Heartbeat HP timeout");
      } else if (now - activeTestStartTime > ACTIVE_TEST_MAX_DURATION) {
        requestStopActiveTest("Hard-cap durasi maksimum tercapai");
      }
    }

    if (activeTestRunning) {
      if (millis() - lastActiveTestSend >= ACTIVE_TEST_INTERVAL) {
        lastActiveTestSend = millis();
        String pid = "300C0" + String(activeTestCylinder) + "00";
        String res = sendOBDWiFi(pid);
        res.replace("\r", " ");
        res.replace("\n", "");
        if (deviceConnected) {
          String msg = "RAW_RES:" + res;
          pTxCharacteristic->setValue(msg.c_str());
          pTxCharacteristic->notify();
        }
        lastValidDataTime = millis();
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return;
    }

    if (o2StreamRunning) {                                            // ⬅️ BARU
      if (millis() - lastO2StreamSend >= O2_STREAM_INTERVAL) {
        lastO2StreamSend = millis();
        String res = sendOBDWiFi("2211180401");
        lastValidDataTime = millis();
        int idx = res.indexOf("621118");
        if (idx != -1 && res.length() >= idx + 8) {
          int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
          currentO2Volt = A * 0.01;
          if (deviceConnected) {
            String msg = "O2:" + String(currentO2Volt, 2);
            pTxCharacteristic->setValue(msg.c_str());
            pTxCharacteristic->notify();
          }
        }
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      return;
    }

    if (pendingRawCommand != "") {
      Serial.println("[BYPASS WIFI] Menembak kode manual...");
      String res = sendOBDWiFi(pendingRawCommand);
      if (deviceConnected) {
        String msg = "RAW_OBD:" + res;
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
      }
      pendingRawCommand = "";
      lastValidDataTime = millis();
      vTaskDelay(200 / portTICK_PERIOD_MS);
      return; 
    }

    int pid = POLL_SEQ[seqIdx];
    seqIdx = (seqIdx + 1) % POLL_SEQ_LEN;
    bool dataBerubah = false;

    if (pid == 0) { 
      String res = sendOBDWiFi("2212010401"); 
      int idx = res.indexOf("621201");
      if (idx != -1 && res.length() >= idx + 10) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        int B = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
        currentRPM = (int)((A * 3200.0) + (B * 12.5));
        lastValidDataTime = millis();
        dataBerubah = true;
      }
    }
    else if (pid == 1) { 
      String res = sendOBDWiFi("2211020401");
      int idx = res.indexOf("621102");
      if (idx != -1 && res.length() >= idx + 8) {
        currentSpeed = (int)(strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) * 2.0);
        lastValidDataTime = millis(); 
        dataBerubah = true;
        if (autoLockEnabled && currentSpeed >= autoLockSpeed && !isLocked && !isRelayActive) {
          digitalWrite(RELAY_PIN, HIGH); 
          relayTriggerTime = millis();  
          isRelayActive = true;         
          isLocked = true;
        }
      }
    }
    else if (pid == 2) { 
      String res = sendOBDWiFi("2211010401");
      int idx = res.indexOf("621101");
      if (idx != -1 && res.length() >= idx + 8) {
        currentTemp = (int)strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) - 50;
        dataBerubah = true;
      }
    }
    else if (pid == 3) { 
      String res = sendOBDWiFi("2212090401");
      int idx = res.indexOf("621209");
      if (idx != -1 && res.length() >= idx + 10) {
        float valA = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        float valB = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
        currentMAF = (valA * 1.28) + ((valB * 1.27) / 255.0);
        dataBerubah = true;
      }
    }
    else if (pid == 4) { 
      String res = sendOBDWiFi("ATRV");
      if (res.indexOf("V") != -1) { res.replace("V", ""); currentVolt = res.toFloat();
      dataBerubah = true; }
    }
    else if (pid == 5) { 
      String res = sendOBDWiFi("2211060401"); 
      int idx = res.indexOf("621106");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentIAT = A - 50; 
        dataBerubah = true;
      }
    }
    else if (pid == 6) { 
      String res = sendOBDWiFi("2211230401");
      int idx = res.indexOf("621123");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentSTFT = A - 100.0; 
        dataBerubah = true;
      }
    }
    else if (pid == 7) { 
      String res = sendOBDWiFi("2211250401");
      int idx = res.indexOf("621125");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentLTFT = A - 100.0;
        dataBerubah = true;
      }
    }
    else if (pid == 8) { 
      String res = sendOBDWiFi("22110A0401");
      int idx = res.indexOf("62110A");
      if (idx != -1 && res.length() >= idx + 8) {
        int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
        currentTiming = 110 - A; 
        dataBerubah = true;
      }
    }
    else if (pid == 9) { 
      String res = sendOBDWiFi("22120E0401");
      res.replace(" ","");
      int idx = res.indexOf("62120E");
      if (idx != -1 && res.length() >= idx + 10) {
        int A = strtol(res.substring(6, 8).c_str(), NULL, 16); 
        int B = strtol(res.substring(8, 10).c_str(), NULL, 16);
        float volt = (A * 1.28) + (B * 1.27 / 255.0);
        float percent = ((volt - 0.78) / (4.48 - 0.78)) * 100.0;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        currentThrottle = (int)percent; 
        dataBerubah = true;
      }
    }
    
    if (dataBerubah) kirimDataKeHP();

    if (isRelayActive && (millis() - relayTriggerTime >= 500)) {
      digitalWrite(RELAY_PIN, LOW); isRelayActive = false;
    }
    
    if (digitalRead(SENSOR_UNLOCK_PIN) == LOW) {
      if (millis() - lastUnlockTime > 1000) {
        if (isLocked) { 
          isLocked = false; 
          kirimDataKeHP(); 
          Serial.println("[LOCK] Pintu dibuka manual.");
        }
        lastUnlockTime = millis();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}