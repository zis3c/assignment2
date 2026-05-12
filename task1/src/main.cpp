#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <time.h>

// ===== Pin mapping =====
static const int DHT_PIN = 4;
static const int GREEN_LED_PIN = 16;
static const int YELLOW_LED_PIN = 17;
static const int RED_LED_PIN = 5;

// ===== Sensor/display =====
#define DHT_TYPE DHT11
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== Wi-Fi + InfluxDB =====
// Replace with your credentials.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// For InfluxDB 2.x URL example: "https://us-east-1-1.aws.cloud2.influxdata.com"
const char *INFLUXDB_URL = "http://YOUR_INFLUXDB_HOST:8086";
const char *INFLUXDB_TOKEN = "YOUR_INFLUXDB_TOKEN";
const char *INFLUXDB_ORG = "YOUR_INFLUXDB_ORG";
const char *INFLUXDB_BUCKET = "YOUR_INFLUXDB_BUCKET";

InfluxDBClient influxClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point telemetryPoint("environment");

const unsigned long SENSOR_PERIOD_MS = 2000;
unsigned long lastSampleMs = 0;

bool syncClock() {
  // Malaysia time (UTC+8), no DST.
  configTzTime("MYT-8", "pool.ntp.org", "time.nis.gov");
  Serial.print("Syncing time");

  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 1700000000 && attempts < 40) {
    Serial.print(".");
    delay(500);
    now = time(nullptr);
    attempts++;
  }
  Serial.println();

  if (now < 1700000000) {
    Serial.println("Time sync failed. Clock not valid.");
    return false;
  }

  Serial.print("Synchronized time: ");
  Serial.println(ctime(&now));
  return true;
}

bool influxConfigReady() {
  // Prevent accidental writes when placeholder values are still in use.
  return strcmp(INFLUXDB_URL, "http://YOUR_INFLUXDB_HOST:8086") != 0 &&
         strcmp(INFLUXDB_TOKEN, "YOUR_INFLUXDB_TOKEN") != 0 &&
         strcmp(INFLUXDB_ORG, "YOUR_INFLUXDB_ORG") != 0 &&
         strcmp(INFLUXDB_BUCKET, "YOUR_INFLUXDB_BUCKET") != 0;
}

void setLedState(float tempC) {
  // Clear all LEDs first, then assert exactly one state LED.
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);

  // Demo-tuned thresholds for easier visible color transitions.
  if (tempC <= 28.0f) {
    digitalWrite(GREEN_LED_PIN, HIGH);
  } else if (tempC > 28.0f && tempC < 29.0f) {
    digitalWrite(YELLOW_LED_PIN, HIGH);
  } else if (tempC >= 29.0f) {
    digitalWrite(RED_LED_PIN, HIGH);
  }
}

void drawOled(float tempC, float hum) {
  // Refresh the full OLED frame with latest telemetry values.
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Env Telemetry");
  display.println("--------------------");
  display.print("Temp: ");
  display.print(tempC, 1);
  display.println(" C");
  display.print("Hum : ");
  display.print(hum, 1);
  display.println(" %");
  display.display();
}

void connectWifi() {
  // Blocking connect keeps networking simple for this assignment prototype.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  setLedState(-100.0f);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED init failed");
    while (true) {
      delay(1000);
    }
  }
  display.clearDisplay();
  display.display();

  dht.begin();
  connectWifi();

  // Clock sync is required so InfluxDB points get valid timestamps.
  syncClock();

  if (influxClient.validateConnection()) {
    Serial.print("InfluxDB connected: ");
    Serial.println(influxClient.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(influxClient.getLastErrorMessage());
  }

  if (!influxConfigReady()) {
    Serial.println("Influx config missing. Set TOKEN/ORG/BUCKET in main.cpp");
  }

  telemetryPoint.addTag("device", "esp32");
  telemetryPoint.addTag("sensor", "dht11");
}

void loop() {
  // Recover Wi-Fi automatically if link drops.
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  // Fixed-rate sampling loop.
  if (millis() - lastSampleMs < SENSOR_PERIOD_MS) {
    delay(20);
    return;
  }
  lastSampleMs = millis();

  float humidity = dht.readHumidity();
  float temperatureC = dht.readTemperature();

  if (isnan(humidity) || isnan(temperatureC)) {
    // Skip publish/display updates when sensor frame is invalid.
    Serial.println("DHT read failed");
    return;
  }

  setLedState(temperatureC);
  drawOled(temperatureC, humidity);

  if (influxConfigReady()) {
    // Write one telemetry point per sample period.
    telemetryPoint.clearFields();
    telemetryPoint.addField("temperature_c", temperatureC);
    telemetryPoint.addField("humidity_pct", humidity);

    if (!influxClient.writePoint(telemetryPoint)) {
      String err = influxClient.getLastErrorMessage();
      // Lightweight retry for transient network timeouts.
      if (err.indexOf("Timeout") >= 0) {
        delay(300);
        if (influxClient.writePoint(telemetryPoint)) {
          Serial.print("Sent(retry) -> Temp: ");
          Serial.print(temperatureC, 1);
          Serial.print(" C, Hum: ");
          Serial.print(humidity, 1);
          Serial.println(" %");
          return;
        }
        err = influxClient.getLastErrorMessage();
      }
      Serial.print("Influx write failed: ");
      Serial.println(err);
    } else {
      Serial.print("Sent -> Temp: ");
      Serial.print(temperatureC, 1);
      Serial.print(" C, Hum: ");
      Serial.print(humidity, 1);
      Serial.println(" %");
    }
  } else {
    Serial.print("Local only -> Temp: ");
    Serial.print(temperatureC, 1);
    Serial.print(" C, Hum: ");
    Serial.print(humidity, 1);
    Serial.println(" %");
  }
}
