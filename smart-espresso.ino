#include "arduino_secrets.h"  // <- Your credentials

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Ticker.h>

// Pins
const int powerOn = 0;
const int pourDoubleCoffe = 2;

// MQTT
const int mqtt_port = 1883;
const char* mqtt_topic_command = "cafeteira/start";
const char* mqtt_topic_metrics = "metrics/cafeteira";

WiFiClient espClient;
PubSubClient client(espClient);

// Watchdog timer
Ticker watchdog;

// State control
enum State { IDLE, WAITING, POURING };
State currentState = IDLE;
unsigned long eventStartTime = 0;
unsigned long lastAliveTime = 0;
unsigned long lastMemoryCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastMQTTAttempt = 0;

// Intervals and timeouts
const unsigned long aliveInterval = 60000;        // 1 minute
const unsigned long memoryCheckInterval = 300000; // 5 minutes
const unsigned long wifiCheckInterval = 30000;    // 30 seconds
const unsigned long mqttRetryInterval = 5000;     // 5 seconds
const unsigned long coffeeWaitTime = 150000;      // 2.5 minutes
const unsigned long stateTimeout = 300000;        // 5 minutes max for any state
const unsigned long wifiTimeout = 30000;          // 30 seconds WiFi connection timeout
const int minFreeHeap = 1000;                     // Minimum free heap before restart

unsigned long coffeePouredCount = 0;
bool systemHealthy = true;

// Watchdog reset function
void resetWatchdog() {
  ESP.wdtFeed();
}

// Safe time comparison that handles millis() overflow
bool timeElapsed(unsigned long startTime, unsigned long interval) {
  unsigned long currentTime = millis();
  return (currentTime - startTime >= interval) || (currentTime < startTime);
}

void setupWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_NAME);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && !timeElapsed(startAttempt, wifiTimeout)) {
    delay(500);
    Serial.print(".");
    ESP.wdtFeed(); // Feed watchdog during connection
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected to WiFi. IP address: ");
    Serial.println(WiFi.localIP());
    systemHealthy = true;
  } else {
    Serial.println();
    Serial.println("WiFi connection failed after timeout, will retry later");
    systemHealthy = false;
  }
}

void checkWiFiConnection() {
  if (timeElapsed(lastWiFiCheck, wifiCheckInterval)) {
    lastWiFiCheck = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Attempting reconnection...");
      setupWiFi();
      
      // If still not connected after multiple attempts, restart
      static int wifiFailCount = 0;
      if (WiFi.status() != WL_CONNECTED) {
        wifiFailCount++;
        if (wifiFailCount >= 5) {
          Serial.println("Multiple WiFi failures, restarting device...");
          ESP.restart();
        }
      } else {
        wifiFailCount = 0; // Reset counter on successful connection
      }
    }
  }
}

void powerOnCafeteria() {
  Serial.println("Powering on cafeteria...");
  digitalWrite(powerOn, LOW);
  delay(700);
  digitalWrite(powerOn, HIGH);
  Serial.println("Cafeteria powered on.");
}

void pourCoffee() {
  Serial.println("Pouring coffee...");
  currentState = POURING;
  
  digitalWrite(pourDoubleCoffe, LOW);
  delay(700);
  digitalWrite(pourDoubleCoffe, HIGH);
  
  Serial.println("Coffee poured.");
  coffeePouredCount++;
  currentState = IDLE;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();
  Serial.print("MQTT message received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (message == "on" && currentState == IDLE) {
    powerOnCafeteria();
    eventStartTime = millis();
    currentState = WAITING;
    Serial.println("Coffee brewing cycle started");
  } else if (message == "on") {
    Serial.print("Command ignored: current state is ");
    Serial.println(currentState == WAITING ? "WAITING" : "POURING");
  } else if (message == "reset") {
    Serial.println("Reset command received");
    currentState = IDLE;
    coffeePouredCount = 0;
  } else if (message == "restart") {
    Serial.println("Restart command received");
    ESP.restart();
  }
}

bool reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  if (timeElapsed(lastMQTTAttempt, mqttRetryInterval)) {
    lastMQTTAttempt = millis();
    
    Serial.println("Attempting MQTT connection...");
    
    // Create unique client ID to avoid conflicts
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("Connected to MQTT broker.");
      client.subscribe(mqtt_topic_command);
      Serial.print("Subscribed to topic: ");
      Serial.println(mqtt_topic_command);
      return true;
    } else {
      Serial.print("Failed MQTT connection, rc=");
      Serial.print(client.state());
      Serial.println(" will retry later");
      return false;
    }
  }
  return false;
}

bool publishMetrics(String payload) {
  if (!client.connected()) {
    Serial.println("MQTT not connected, cannot publish metrics");
    return false;
  }
  
  // Try publishing with retries
  for (int attempt = 0; attempt < 3; attempt++) {
    if (client.publish(mqtt_topic_metrics, payload.c_str())) {
      Serial.print("Published metrics to ");
      Serial.print(mqtt_topic_metrics);
      Serial.print(": ");
      Serial.println(payload);
      return true;
    }
    delay(100);
  }
  
  Serial.println("Failed to publish metrics after retries");
  return false;
}

void checkMemory() {
  if (timeElapsed(lastMemoryCheck, memoryCheckInterval)) {
    lastMemoryCheck = millis();
    
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.print("Free heap: ");
    Serial.print(freeHeap);
    Serial.println(" bytes");
    
    if (freeHeap < minFreeHeap) {
      Serial.println("Critical low memory detected, restarting device...");
      delay(1000);
      ESP.restart();
    }
  }
}

void checkStateTimeout() {
  if (currentState != IDLE && timeElapsed(eventStartTime, stateTimeout)) {
    Serial.print("State timeout detected in state: ");
    Serial.println(currentState == WAITING ? "WAITING" : "POURING");
    Serial.println("Resetting to IDLE state");
    currentState = IDLE;
  }
}

void handleCoffeeProcess() {
  if (currentState == WAITING && timeElapsed(eventStartTime, coffeeWaitTime)) {
    pourCoffee();
  }
}

void sendPeriodicMetrics() {
  if (timeElapsed(lastAliveTime, aliveInterval)) {
    lastAliveTime = millis();

    long rssi = WiFi.RSSI();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t uptime = millis() / 1000; // Uptime in seconds
    
    // Convert state to numeric value
    int stateValue = 0;  // IDLE = 0
    if (currentState == WAITING) stateValue = 1;
    else if (currentState == POURING) stateValue = 2;
    
    // Convert boolean connections to numeric
    int wifiConnected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    int mqttConnected = client.connected() ? 1 : 0;

    String payload = "{";
    payload += "\"alive\":1,";
    payload += "\"coffee_poured\":" + String(coffeePouredCount) + ",";
    payload += "\"rssi\":" + String(rssi) + ",";
    payload += "\"free_heap\":" + String(freeHeap) + ",";
    payload += "\"uptime\":" + String(uptime) + ",";
    payload += "\"state\":" + String(stateValue) + ",";
    payload += "\"wifi_connected\":" + String(wifiConnected) + ",";
    payload += "\"mqtt_connected\":" + String(mqttConnected);
    payload += "}";

    if (!publishMetrics(payload)) {
      Serial.println("Metrics publishing failed, connection issues detected");
      systemHealthy = false;
    } else {
      systemHealthy = true;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Smart Espresso Machine Starting...");

  // Initialize pins
  pinMode(powerOn, OUTPUT);
  pinMode(pourDoubleCoffe, OUTPUT);
  digitalWrite(powerOn, HIGH);
  digitalWrite(pourDoubleCoffe, HIGH);

  // Setup watchdog timer (30 second timeout)
  watchdog.attach(30, resetWatchdog);
  
  // Initialize WiFi
  setupWiFi();

  // Setup MQTT
  client.setServer(MQTT_BROKER, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(60); // 60 second keepalive

  // Initialize timing variables
  lastAliveTime = millis();
  lastMemoryCheck = millis();
  lastWiFiCheck = millis();
  lastMQTTAttempt = 0;

  Serial.println("Setup complete. System ready.");
  Serial.print("Free heap at startup: ");
  Serial.println(ESP.getFreeHeap());
}

void loop() {
  // Feed the watchdog
  ESP.wdtFeed();
  
  // Check WiFi connection periodically
  checkWiFiConnection();
  
  // Handle MQTT connection
  if (!client.connected() && WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }
  
  // Process MQTT messages
  if (client.connected()) {
    client.loop();
  }
  
  // Handle coffee brewing process
  handleCoffeeProcess();
  
  // Check for state timeouts
  checkStateTimeout();
  
  // Send periodic metrics
  sendPeriodicMetrics();
  
  // Check memory usage
  checkMemory();
  
  // Small delay to prevent overwhelming the system
  delay(100);
}
