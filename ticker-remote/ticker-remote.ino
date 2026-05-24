#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>

#define BUTTON_PIN    0
#define LED_PIN       LED_BUILTIN   // GPIO 48
#define DEVICE_NAME   "TickerDisplay"
#define SERVICE_UUID  "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHAR_UUID     "BEB5483E-36E1-4688-B7F5-EA07361B26A8"

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pChar = nullptr;
BLEAdvertisedDevice* targetDevice = nullptr;
bool connected = false;
bool doScan = true;

// ── 스캔 콜백 ─────────────────────────────────────────
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (dev.getName() == DEVICE_NAME) {
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(dev);
      Serial.println("TickerDisplay found.");
    }
  }
};

// ── 연결 콜백 ─────────────────────────────────────────
class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    connected = true;
    digitalWrite(LED_PIN, HIGH);  // 연결됨 → LED 켜짐
    Serial.println("BLE connected.");
  }
  void onDisconnect(BLEClient*) override {
    connected = false;
    digitalWrite(LED_PIN, LOW);   // 끊김 → LED 꺼짐
    doScan = true;
    Serial.println("BLE disconnected. Scanning...");
  }
};

// ── 서버 연결 시도 ────────────────────────────────────
bool connectToServer() {
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());
  pClient->connect(targetDevice);

  BLERemoteService* pSvc = pClient->getService(BLEUUID(SERVICE_UUID));
  if (!pSvc) {
    Serial.println("Service not found.");
    pClient->disconnect();
    return false;
  }

  pChar = pSvc->getCharacteristic(BLEUUID(CHAR_UUID));
  if (!pChar) {
    Serial.println("Characteristic not found.");
    pClient->disconnect();
    return false;
  }

  doScan = false;
  return true;
}

// ── 스캔 실행 ─────────────────────────────────────────
void startScan() {
  Serial.println("Scanning for TickerDisplay...");
  targetDevice = nullptr;
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->start(5, false);  // 5초 블로킹 스캔
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  BLEDevice::init("TickerRemote");
  startScan();
}

bool lastBtn = HIGH;

void loop() {
  // 재연결 필요 시 스캔
  if (doScan) {
    startScan();
    if (targetDevice) {
      connectToServer();
    } else {
      delay(2000);  // 못 찾으면 잠시 후 재시도
    }
    return;
  }

  // 버튼 누르면 BLE write
  bool btn = digitalRead(BUTTON_PIN);
  if (lastBtn == HIGH && btn == LOW) {
    if (connected && pChar) {
      // 버튼 누를 때 LED 짧게 깜박
      digitalWrite(LED_PIN, LOW);
      delay(80);
      digitalWrite(LED_PIN, HIGH);

      uint8_t val = 1;
      pChar->writeValue(&val, 1);
      Serial.println("Button sent.");
    } else {
      Serial.println("Not connected.");
    }
    delay(220);  // 디바운스 (80ms LED off + 220ms = 300ms 총)
  }
  lastBtn = btn;
}
