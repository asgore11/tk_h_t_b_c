#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ---------- Cấu hình ----------
#define MQ2_PIN   34
#define DHT_PIN   15
#define FAN_PIN   26
#define PUMP_PIN  27

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

#define T_DATA  "nhom1/baocay/data"      // JSON {"gas":..,"temp":..}
#define T_FAN   "nhom1/baocay/fan"       // ON / OFF / AUTO
#define T_PUMP  "nhom1/baocay/pump"      // ON / OFF / AUTO

#define GAS_THRESHOLD   2000   // ADC 0-4095
#define TEMP_THRESHOLD  50.0   // °C

DHT dht(DHT_PIN, DHT22);
WiFiClient espClient;
PubSubClient mqtt(espClient);

struct SensorData { int gas; float temp; };

QueueHandle_t dataQueue;   // Task đọc -> Task truyền
QueueHandle_t alertQueue;  // Task đọc -> Task cảnh báo

// 0 = AUTO, 1 = ON (manual), 2 = OFF (manual)
volatile int fanMode = 0, pumpMode = 0;

void onMessage(char* topic, byte* payload, unsigned int len) {
  String m; for (unsigned i = 0; i < len; i++) m += (char)payload[i];
  int v = (m == "ON") ? 1 : (m == "OFF") ? 2 : 0;
  if (String(topic) == T_FAN)  fanMode  = v;
  if (String(topic) == T_PUMP) pumpMode = v;
}

// ---------- Task truyền data (Core 0) ----------
void taskTransmit(void* pv) {
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) { vTaskDelay(500 / portTICK_PERIOD_MS); }
  Serial.println("WiFi OK: " + WiFi.localIP().toString());
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  SensorData d;
  for (;;) {
    if (!mqtt.connected()) {
      String id = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(id.c_str())) {
        mqtt.subscribe(T_FAN); mqtt.subscribe(T_PUMP);
        Serial.println("MQTT OK");
      } else { vTaskDelay(2000 / portTICK_PERIOD_MS); continue; }
    }
    mqtt.loop();
    if (xQueueReceive(dataQueue, &d, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"gas\":%d,\"temp\":%.1f}", d.gas, d.temp);
      mqtt.publish(T_DATA, buf);
    }
  }
}

// ---------- Task đọc data (Core 1) ----------
void taskRead(void* pv) {
  SensorData d;
  for (;;) {
    d.gas  = analogRead(MQ2_PIN);
    d.temp = dht.readTemperature();
    if (!isnan(d.temp)) {
      Serial.printf("Gas: %d | Temp: %.1f C\n", d.gas, d.temp);
      xQueueSend(dataQueue,  &d, 0);
      xQueueSend(alertQueue, &d, 0);
    } else Serial.println("Loi doc DHT22");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ---------- Task cảnh báo (Core 1) ----------
void taskAlert(void* pv) {
  SensorData d = {0, 0};
  for (;;) {
    xQueueReceive(alertQueue, &d, 200 / portTICK_PERIOD_MS);  // timeout để vẫn nhận lệnh manual
    bool fan  = (fanMode  == 0) ? (d.gas  > GAS_THRESHOLD)  : (fanMode  == 1);
    bool pump = (pumpMode == 0) ? (d.temp > TEMP_THRESHOLD) : (pumpMode == 1);
    digitalWrite(FAN_PIN,  fan);
    digitalWrite(PUMP_PIN, pump);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_PIN, OUTPUT); pinMode(PUMP_PIN, OUTPUT);
  dht.begin();
  dataQueue  = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(10, sizeof(SensorData));
  xTaskCreatePinnedToCore(taskTransmit, "Transmit", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskRead,     "Read",     4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskAlert,    "Alert",    4096, NULL, 3, NULL, 1);
}

void loop() { vTaskDelete(NULL); }
