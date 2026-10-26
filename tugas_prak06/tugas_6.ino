#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial jaringan Wi-Fi
const char* ssid = "Keenan";
const char* password = "yesirahmawati123";

// Konfigurasi broker MQTT publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782115_Full";

// Pembagian topik komunikasi MQTT
const char* topicPublish   = "kampus/labiot/25782115/telemetri";
const char* topicSubscribe = "kampus/labiot/25782115/kendali";
const char* topicAlert     = "kampus/labiot/25782115/alert";
const char* topicStatus    = "kampus/labiot/25782115/status";

// Alokasi pin perangkat keras NodeMCU
const byte dhtPin = 2;
const byte ledPin = 5;
#define DHTTYPE DHT22

// Inisialisasi perangkat dan klien MQTT
DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// Pengaturan interval telemetri
unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000;

// Pengaturan percobaan ulang Wi-Fi
unsigned long lastWiFiAttempt = 0;
const unsigned long wifiRetryInterval = 10000;

// Pengaturan percobaan ulang MQTT
unsigned long lastMQTTAttempt = 0;
const unsigned long mqttRetryInterval = 5000;

// Pengaturan percobaan ulang alert
unsigned long lastAlertAttempt = 0;
const unsigned long alertRetryInterval = 5000;

// Ambang batas suhu bahaya
const float batasSuhu = 31.0;

// Status keselamatan dan validitas sensor
bool dangerActive = false;
bool alertLatched = false;
bool sensorValid = false;
bool wifiConnectedPreviously = false;

// Penyimpan suhu terakhir
float suhuTerakhir = NAN;

// Pesan Last Will dan status ONLINE
const char* willMessage = "{\"status\":\"OFFLINE\",\"keterangan\":\"Perangkat Terputus Tak Terduga\"}";
const char* onlineMessage = "{\"status\":\"ONLINE\",\"keterangan\":\"Perangkat Beroperasi Normal\"}";

// Memproses perintah kendali LED dari MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  char incomingBuffer[length + 1];
  memcpy(incomingBuffer, payload, length);
  incomingBuffer[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n",
                topic, incomingBuffer);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incomingBuffer);

  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  if (String(topic) == topicSubscribe && doc.containsKey("led")) {
    int val = doc["led"];

    if (val != 0 && val != 1) {
      Serial.println("[ERROR] Perintah LED hanya boleh bernilai 0 atau 1!");
    } else if (!sensorValid) {
      Serial.println("[PERINGATAN] Kendali manual ditolak karena data sensor belum valid!");
    } else if (dangerActive && val == 0) {
      digitalWrite(ledPin, HIGH);
      Serial.println("[PERINGATAN] LED tidak dapat dipadamkan karena suhu BAHAYA!");
    } else {
      digitalWrite(ledPin, val == 1 ? HIGH : LOW);
      Serial.printf("--> [INDIKATOR] Status LED diubah: %s\n",
                    val == 1 ? "MENYALA" : "PADAM");
    }
  }

  Serial.println("----------------------------------------");
}

// Memelihara koneksi Wi-Fi tanpa delay
void maintainWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnectedPreviously) {
      wifiConnectedPreviously = true;
      Serial.println("Wi-Fi Terkoneksi!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
    }
  } else {
    wifiConnectedPreviously = false;

    if (now - lastWiFiAttempt >= wifiRetryInterval) {
      lastWiFiAttempt = now;
      Serial.println("Menghubungkan kembali ke Wi-Fi...");
      WiFi.begin(ssid, password);
    }
  }
}

// Menghubungkan MQTT dan mendaftarkan Last Will
void reconnect() {
  unsigned long now = millis();

  if (now - lastMQTTAttempt < mqttRetryInterval) {
    return;
  }

  lastMQTTAttempt = now;

  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    Serial.print("Menghubungkan kembali ke Broker...");

    if (client.connect(clientId, topicStatus, 1, true, willMessage)) {
      Serial.println(" Sukses Terhubung!");

      client.subscribe(topicSubscribe);
      Serial.printf("Subskripsi Topik: %s\n", topicSubscribe);

      client.publish(topicStatus, onlineMessage, true);
      Serial.println("[STATUS] Perangkat ONLINE");

      lastAlertAttempt = 0;
    } else {
      Serial.printf("Gagal (rc=%d). Akan mencoba kembali...\n",
                    client.state());
    }
  }
}

// Mendeteksi suhu bahaya, menyalakan LED, dan menerbitkan alert
void processSafety(float t) {
  bool previousDanger = dangerActive;
  dangerActive = t > batasSuhu;

  if (dangerActive) {
    digitalWrite(ledPin, HIGH);

    // Memulai siklus alert baru ketika suhu baru melewati batas
    if (!previousDanger) {
      alertLatched = false;
      lastAlertAttempt = 0;
    }

    unsigned long now = millis();

    // Alert pertama langsung dicoba; percobaan ulang dibatasi interval
    if ((!alertLatched ||
         now - lastAlertAttempt >= alertRetryInterval) &&
        client.connected()) {

      StaticJsonDocument<256> alertDoc;
      alertDoc["status"] = "BAHAYA";
      alertDoc["parameter"] = "OVERHEAT";
      alertDoc["nilai_suhu"] = t;

      char alertBuffer[256];
      serializeJson(alertDoc, alertBuffer);

      if (client.publish(topicAlert, alertBuffer)) {
        Serial.printf("[ALERT TERKIRIM] %s\n", alertBuffer);
        alertLatched = true;
        lastAlertAttempt = millis();
      } else {
        Serial.println("[ERROR] Gagal mengirim alert!");
      }
    }
  } else {
    // Mengizinkan alert baru setelah suhu kembali normal
    alertLatched = false;
    lastAlertAttempt = 0;
  }
}

// Inisialisasi sensor, Wi-Fi, dan MQTT tanpa menunggu koneksi
void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  lastWiFiAttempt = millis();

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  client.setKeepAlive(30);
  client.setBufferSize(512);

  Serial.println("Sistem Telemetri dan Kendali MQTT dimulai.");
}

// Loop utama untuk koneksi, keselamatan, dan telemetri berkala
void loop() {
  maintainWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnect();
    }

    client.loop();
  }

  unsigned long now = millis();

  if (now - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      sensorValid = true;
      suhuTerakhir = t;

      processSafety(t);

      StaticJsonDocument<512> docOut;
      docOut["node_id"] = clientId;
      docOut["suhu"] = t;
      docOut["kelembapan"] = h;
      docOut["status_led"] = digitalRead(ledPin);
      docOut["kondisi_suhu"] = t > batasSuhu ? "PANAS" : "NORMAL";
      docOut["uptime_detik"] = millis() / 1000;
      docOut["ip_lokal"] = WiFi.localIP().toString();

      char buffer[512];
      serializeJson(docOut, buffer);

      if (client.connected()) {
        bool success = client.publish(topicPublish, buffer);

        if (success) {
          Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
        } else {
          Serial.println("[ERROR] Gagal memublikasikan telemetri!");
        }
      }
    } else {
      sensorValid = false;
      Serial.println("[ERROR] Gagal membaca sensor DHT22!");
    }
  }
}