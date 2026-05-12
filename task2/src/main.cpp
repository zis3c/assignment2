#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ===================== EDIT THESE SETTINGS =====================
// WiFi used by ESP32.
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT broker + client identity.
static const char *MQTT_BROKER = "YOUR_MQTT_BROKER_HOST";
static const uint16_t MQTT_PORT = 1883;
static const char *MQTT_USERNAME = "";
static const char *MQTT_PASSWORD = "";
static const char *DEVICE_NAME = "esp32-koden-security";
// ==============================================================

// Hardware pins from the task sheet.
static const uint8_t PIR_PIN = 19;
static const uint8_t RELAY_PIN = 18;

// Relay module logic can vary. Most 1-channel modules are active LOW.
static const bool RELAY_ACTIVE_STATE = LOW;
static const bool RELAY_INACTIVE_STATE = HIGH;
static const bool PIR_ACTIVE_STATE = HIGH;

// MQTT topics used by ESP32 and Node-RED dashboard.
static const char *TOPIC_MOTION_STATUS = "iot/security/motion/status";
static const char *TOPIC_MOTION_RAW = "iot/security/motion/raw";
static const char *TOPIC_RELAY_STATE = "iot/security/relay/state";
static const char *TOPIC_RELAY_SET = "iot/security/relay/set";
static const char *TOPIC_CONTROL_MODE = "iot/security/control/mode";

static const unsigned long WIFI_RETRY_DELAY_MS = 500;
static const unsigned long MQTT_RETRY_DELAY_MS = 2000;
static const unsigned long LOOP_DELAY_MS = 150;
static const unsigned long PUBLISH_HEARTBEAT_MS = 5000;
static const unsigned long PIR_WARMUP_MS = 30000;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

enum ControlMode {
  MODE_AUTO,
  MODE_FORCE_OFF,
  MODE_FORCE_ON
};

bool pirMotionDetected = false;
bool relayEnabled = false;
ControlMode controlMode = MODE_AUTO;
unsigned long lastHeartbeatMs = 0;
unsigned long startupMs = 0;

const char *getMotionText(bool motionDetected) {
  return motionDetected ? "Intruder Detected" : "Safe";
}

const char *getModeText(ControlMode mode) {
  switch (mode) {
    case MODE_AUTO:
      return "AUTO";
    case MODE_FORCE_OFF:
      return "FORCE_OFF";
    case MODE_FORCE_ON:
      return "FORCE_ON";
    default:
      return "UNKNOWN";
  }
}

void setRelay(bool enabled) {
  // Persist software state and drive physical relay pin together.
  relayEnabled = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ACTIVE_STATE : RELAY_INACTIVE_STATE);
}

// Publish latest state to retained topics so dashboard can recover state.
void publishState(bool forcePublish = false) {
  if (!mqttClient.connected()) {
    return;
  }

  static bool lastPublishedMotion = false;
  static bool lastPublishedRelay = false;
  static ControlMode lastPublishedMode = MODE_AUTO;

  if (!forcePublish &&
      lastPublishedMotion == pirMotionDetected &&
      lastPublishedRelay == relayEnabled &&
      lastPublishedMode == controlMode) {
    return;
  }

  mqttClient.publish(TOPIC_MOTION_STATUS, getMotionText(pirMotionDetected), true);
  mqttClient.publish(TOPIC_MOTION_RAW, pirMotionDetected ? "1" : "0", true);
  mqttClient.publish(TOPIC_RELAY_STATE, relayEnabled ? "ON" : "OFF", true);
  mqttClient.publish(TOPIC_CONTROL_MODE, getModeText(controlMode), true);

  lastPublishedMotion = pirMotionDetected;
  lastPublishedRelay = relayEnabled;
  lastPublishedMode = controlMode;
}

// Core behavior:
// AUTO -> relay follows PIR, FORCE_* -> manual override from MQTT.
void applySecurityLogic() {
  if (controlMode == MODE_FORCE_ON) {
    setRelay(true);
  } else if (controlMode == MODE_FORCE_OFF) {
    setRelay(false);
  } else {
    setRelay(pirMotionDetected);
  }
}

void printSystemState() {
  Serial.print("Motion: ");
  Serial.print(getMotionText(pirMotionDetected));
  Serial.print(" | Relay: ");
  Serial.print(relayEnabled ? "ON" : "OFF");
  Serial.print(" | Mode: ");
  Serial.println(getModeText(controlMode));
}

void handleRelayCommand(const String &command) {
  // Accept common command aliases from dashboard or manual MQTT publish.
  if (command == "ON" || command == "1" || command == "TRUE") {
    controlMode = MODE_FORCE_ON;
  } else if (command == "OFF" || command == "0" || command == "FALSE") {
    controlMode = MODE_FORCE_OFF;
  } else if (command == "AUTO") {
    controlMode = MODE_AUTO;
  } else {
    Serial.print("Unknown MQTT command: ");
    Serial.println(command);
    return;
  }

  applySecurityLogic();
  publishState(true);
  printSystemState();
}

// Handles subscribed MQTT commands.
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  // Build command string from raw MQTT payload bytes.
  String message;
  message.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    message += static_cast<char>(payload[i]);
  }

  message.trim();
  message.toUpperCase();

  Serial.print("MQTT message on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  if (String(topic) == TOPIC_RELAY_SET) {
    handleRelayCommand(message);
  }
}

// Blocking WiFi reconnect loop.
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  // Blocking reconnect keeps security logic deterministic for this project.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// Blocking MQTT reconnect loop and re-subscribe.
void connectMqtt() {
  // Reconnect loop also re-subscribes command topic after broker reconnect.
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT broker...");

    bool connected = false;
    if (strlen(MQTT_USERNAME) == 0) {
      connected = mqttClient.connect(DEVICE_NAME);
    } else {
      connected = mqttClient.connect(DEVICE_NAME, MQTT_USERNAME, MQTT_PASSWORD);
    }

    if (connected) {
      Serial.println("connected");
      mqttClient.subscribe(TOPIC_RELAY_SET);
      publishState(true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(". Retrying...");
      delay(MQTT_RETRY_DELAY_MS);
    }
  }
}

void setup() {
  Serial.begin(115200);
  startupMs = millis();

  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  connectWiFi();

  // Configure broker endpoint and incoming message handler.
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.println("Smart security system started.");
  Serial.println("Waiting for PIR sensor warm-up...");
  printSystemState();
}

void loop() {
  // Keep links alive.
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }

  // Required to process incoming MQTT packets and keep connection alive.
  mqttClient.loop();

  // Ignore PIR during warm-up window to reduce false triggers.
  if (millis() - startupMs < PIR_WARMUP_MS) {
    delay(LOOP_DELAY_MS);
    return;
  }

  // Recompute state only on PIR edge changes.
  bool currentPirState = digitalRead(PIR_PIN) == PIR_ACTIVE_STATE;
  if (currentPirState != pirMotionDetected) {
    pirMotionDetected = currentPirState;
    applySecurityLogic();
    publishState(true);
    printSystemState();
  }

  // Periodic heartbeat publish for dashboard freshness.
  if (millis() - lastHeartbeatMs >= PUBLISH_HEARTBEAT_MS) {
    publishState(true);
    lastHeartbeatMs = millis();
  }

  delay(LOOP_DELAY_MS);
}
