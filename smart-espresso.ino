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

// RTC Memory structure for persistent data across reboots
struct RTCData {
  uint32_t crc32;
  uint32_t totalRestarts;
  uint32_t watchdogRestarts;
  uint32_t memoryRestarts;
  uint32_t wifiRestarts;
  uint32_t mqttRestarts;
  uint32_t healthRestarts;
  uint32_t userRestarts;
  uint32_t unknownRestarts;
  uint32_t lastRestartReason;
  uint32_t coffeePouredTotal; // Also persist coffee count
};

RTCData rtcData;

// Restart reason codes
enum RestartReason {
  RESTART_UNKNOWN = 0,
  RESTART_USER = 1,
  RESTART_WATCHDOG = 2,
  RESTART_MEMORY = 3,
  RESTART_WIFI = 4,
  RESTART_MQTT = 5,
  RESTART_HEALTH = 6,
  RESTART_POWER_ON = 7
};

// Watchdog and health monitoring
Ticker watchdog;
Ticker healthCheck;
unsigned long lastSuccessfulMetrics = 0;
unsigned long lastMQTTMessage = 0;
unsigned long bootTime = 0;
bool forceRestart = false;
RestartReason pendingRestartReason = RESTART_UNKNOWN;

// State control
enum State { IDLE, WAITING, POURING };
State currentState = IDLE;
unsigned long eventStartTime = 0;
unsigned long lastAliveTime = 0;
unsigned long lastMemoryCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long lastConnectionHealth = 0;

// Intervals and timeouts
const unsigned long aliveInterval = 60000;        // 1 minute
const unsigned long memoryCheckInterval = 300000; // 5 minutes
const unsigned long wifiCheckInterval = 30000;    // 30 seconds
const unsigned long mqttRetryInterval = 5000;     // 5 seconds
const unsigned long coffeeWaitTime = 150000;      // 2.5 minutes
const unsigned long stateTimeout = 300000;        // 5 minutes max for any state
const unsigned long wifiTimeout = 30000;          // 30 seconds WiFi connection timeout
const unsigned long healthCheckInterval = 120000; // 2 minutes
const unsigned long maxSilentTime = 300000;       // 5 minutes without successful metrics
const unsigned long maxMQTTSilentTime = 600000;   // 10 minutes without MQTT activity
const int minFreeHeap = 1000;                     // Minimum free heap before restart

unsigned long coffeePouredCount = 0;
bool systemHealthy = true;
int consecutiveFailures = 0;
int wifiReconnectCount = 0;
int mqttReconnectCount = 0;

// CRC32 calculation for RTC data validation
uint32_t calculateCRC32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffff;
  while (length--) {
    uint8_t c = *data++;
    for (uint32_t i = 0x80; i > 0; i >>= 1) {
      bool bit = crc & 0x80000000;
      if (c & i) {
        bit = !bit;
      }
      crc <<= 1;
      if (bit) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}

// Read data from RTC memory
bool readRTCData() {
  if (ESP.rtcUserMemoryRead(0, (uint32_t*) &rtcData, sizeof(rtcData))) {
    uint32_t crcOfData = calculateCRC32(((uint8_t*) &rtcData) + 4, sizeof(rtcData) - 4);
    if (crcOfData == rtcData.crc32) {
      Serial.println("RTC data is valid");
      return true;
    } else {
      Serial.println("RTC data CRC mismatch, initializing");
    }
  } else {
    Serial.println("Failed to read RTC data");
  }
  return false;
}

// Write data to RTC memory
void writeRTCData() {
  rtcData.crc32 = calculateCRC32(((uint8_t*) &rtcData) + 4, sizeof(rtcData) - 4);
  ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcData, sizeof(rtcData));
}

// Initialize RTC data structure
void initRTCData() {
  rtcData.totalRestarts = 0;
  rtcData.watchdogRestarts = 0;
  rtcData.memoryRestarts = 0;
  rtcData.wifiRestarts = 0;
  rtcData.mqttRestarts = 0;
  rtcData.healthRestarts = 0;
  rtcData.userRestarts = 0;
  rtcData.unknownRestarts = 0;
  rtcData.lastRestartReason = RESTART_POWER_ON;
  rtcData.coffeePouredTotal = 0;
  writeRTCData();
}

// Record restart and increment counters
void recordRestart(RestartReason reason) {
  rtcData.totalRestarts++;
  rtcData.lastRestartReason = reason;
  
  switch (reason) {
    case RESTART_USER:
      rtcData.userRestarts++;
      break;
    case RESTART_WATCHDOG:
      rtcData.watchdogRestarts++;
      break;
    case RESTART_MEMORY:
      rtcData.memoryRestarts++;
      break;
    case RESTART_WIFI:
      rtcData.wifiRestarts++;
      break;
    case RESTART_MQTT:
      rtcData.mqttRestarts++;
      break;
    case RESTART_HEALTH:
      rtcData.healthRestarts++;
      break;
    default:
      rtcData.unknownRestarts++;
      break;
  }
  
  // Also save current coffee count
  rtcData.coffeePouredTotal = coffeePouredCount;
  
  writeRTCData();
  
  Serial.print("Recording restart reason: ");
  Serial.println(reason);
  Serial.print("Total restarts: ");
  Serial.println(rtcData.totalRestarts);
}

// Perform restart with reason tracking
void performRestart(RestartReason reason) {
  Serial.print("Performing restart due to: ");
  Serial.println(reason);
  recordRestart(reason);
  delay(1000);
  ESP.restart();
}

// Watchdog reset function - more aggressive
void resetWatchdog() {
  ESP.wdtFeed();
  ESP.wdtEnable(WDTO_8S); // 8 second hardware watchdog
}

// Health check function
void performHealthCheck() {
  unsigned long now = millis();
  
  // Check if we haven't sent metrics successfully in too long
  if (timeElapsed(lastSuccessfulMetrics, maxSilentTime)) {
    Serial.println("HEALTH: No successful metrics for too long, forcing restart");
    pendingRestartReason = RESTART_HEALTH;
    forceRestart = true;
    return;
  }
  
  // Check if MQTT has been silent for too long
  if (timeElapsed(lastMQTTMessage, maxMQTTSilentTime)) {
    Serial.println("HEALTH: MQTT silent for too long, forcing restart");
    pendingRestartReason = RESTART_MQTT;
    forceRestart = true;
    return;
  }
  
  // Check for excessive reconnection attempts
  if (wifiReconnectCount > 20) {
    Serial.println("HEALTH: Too many WiFi reconnection attempts, forcing restart");
    pendingRestartReason = RESTART_WIFI;
    forceRestart = true;
    return;
  }
  
  if (mqttReconnectCount > 50) {
    Serial.println("HEALTH: Too many MQTT reconnection attempts, forcing restart");
    pendingRestartReason = RESTART_MQTT;
    forceRestart = true;
    return;
  }
  
  // Reset counters periodically if system is healthy
  if (now - bootTime > 86400000) { // 24 hours
    wifiReconnectCount = 0;
    mqttReconnectCount = 0;
    bootTime = now;
    Serial.println("HEALTH: Daily counter reset");
  }
}

// Safe time comparison that handles millis() overflow
bool timeElapsed(unsigned long startTime, unsigned long interval) {
  unsigned long currentTime = millis();
  return (currentTime - startTime >= interval) || (currentTime < startTime);
}

void setupWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_NAME);

  // Disconnect first to ensure clean state
  WiFi.disconnect();
  delay(100);
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false); // Don't save WiFi config to flash
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
    Serial.print("Signal strength: ");
    Serial.println(WiFi.RSSI());
    systemHealthy = true;
    consecutiveFailures = 0;
  } else {
    Serial.println();
    Serial.println("WiFi connection failed after timeout, will retry later");
    systemHealthy = false;
    consecutiveFailures++;
    wifiReconnectCount++;
  }
}

void checkWiFiConnection() {
  if (timeElapsed(lastWiFiCheck, wifiCheckInterval)) {
    lastWiFiCheck = millis();

    // More thorough WiFi check
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
      Serial.println("WiFi disconnected or invalid IP. Attempting reconnection...");
      setupWiFi();

      // If still not connected after multiple attempts, restart
      if (WiFi.status() != WL_CONNECTED) {
        consecutiveFailures++;
        if (consecutiveFailures >= 5) {
          Serial.println("Multiple WiFi failures, forcing restart...");
          pendingRestartReason = RESTART_WIFI;
          forceRestart = true;
        }
      } else {
        consecutiveFailures = 0; // Reset counter on successful connection
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
  lastMQTTMessage = millis(); // Update last MQTT activity
  
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
    // Reset restart counters
    initRTCData();
  } else if (message == "restart") {
    Serial.println("Restart command received");
    pendingRestartReason = RESTART_USER;
    forceRestart = true;
  } else if (message == "ping") {
    // Health check command
    Serial.println("Ping received - system responsive");
  }
}

bool reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot connect to MQTT");
    return false;
  }

  if (timeElapsed(lastMQTTAttempt, mqttRetryInterval)) {
    lastMQTTAttempt = millis();
    mqttReconnectCount++;

    Serial.print("Attempting MQTT connection (attempt #");
    Serial.print(mqttReconnectCount);
    Serial.println(")...");

    // Create unique client ID to avoid conflicts
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);

    // Disconnect first to ensure clean state
    client.disconnect();
    delay(100);

    // Set clean session and proper timeouts
    if (client.connect(clientId.c_str(), NULL, NULL, NULL, 0, false, NULL, true)) {
      Serial.println("Connected to MQTT broker.");
      client.subscribe(mqtt_topic_command);
      Serial.print("Subscribed to topic: ");
      Serial.println(mqtt_topic_command);
      lastMQTTMessage = millis();
      
      // Test the connection with a small publish
      String testPayload = "{\"test\":1}";
      if (client.publish("test/cafeteira", testPayload.c_str(), false)) {
        Serial.println("MQTT connection test successful");
      } else {
        Serial.println("MQTT connection test failed");
      }
      
      return true;
    } else {
      Serial.print("Failed MQTT connection, rc=");
      Serial.print(client.state());
      Serial.print(" (");
      switch(client.state()) {
        case -4: Serial.print("MQTT_CONNECTION_TIMEOUT"); break;
        case -3: Serial.print("MQTT_CONNECTION_LOST"); break;
        case -2: Serial.print("MQTT_CONNECT_FAILED"); break;
        case -1: Serial.print("MQTT_DISCONNECTED"); break;
        case 0: Serial.print("MQTT_CONNECTED"); break;
        case 1: Serial.print("MQTT_CONNECT_BAD_PROTOCOL"); break;
        case 2: Serial.print("MQTT_CONNECT_BAD_CLIENT_ID"); break;
        case 3: Serial.print("MQTT_CONNECT_UNAVAILABLE"); break;
        case 4: Serial.print("MQTT_CONNECT_BAD_CREDENTIALS"); break;
        case 5: Serial.print("MQTT_CONNECT_UNAUTHORIZED"); break;
        default: Serial.print("UNKNOWN"); break;
      }
      Serial.println(") will retry later");
      return false;
    }
  }
  return false;
}

bool publishMetrics(String payload) {
  // Check connections first
  Serial.print("Publishing metrics - WiFi: ");
  Serial.print(WiFi.status() == WL_CONNECTED ? "OK" : "FAIL");
  Serial.print(", MQTT: ");
  Serial.print(client.connected() ? "OK" : "FAIL");
  Serial.print(" (state: ");
  Serial.print(client.state());
  Serial.println(")");

  if (!client.connected()) {
    Serial.println("MQTT not connected, cannot publish metrics");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot publish metrics");
    return false;
  }

  Serial.print("Payload size: ");
  Serial.print(payload.length());
  Serial.println(" bytes");
  Serial.print("Payload: ");
  Serial.println(payload);

  // Try publishing with retries
  for (int attempt = 0; attempt < 3; attempt++) {
    Serial.print("Publish attempt ");
    Serial.print(attempt + 1);
    Serial.print("/3... ");
    
    bool result = client.publish(mqtt_topic_metrics, payload.c_str(), false);
    
    if (result) {
      Serial.println("SUCCESS");
      lastSuccessfulMetrics = millis();
      return true;
    } else {
      Serial.print("FAILED (MQTT state: ");
      Serial.print(client.state());
      Serial.println(")");
      
      // Check if connection was lost
      if (!client.connected()) {
        Serial.println("MQTT connection lost during publish");
        break;
      }
    }
    
    delay(200);
    ESP.wdtFeed();
  }

  Serial.println("All publish attempts failed");
  return false;
}

void checkMemory() {
  if (timeElapsed(lastMemoryCheck, memoryCheckInterval)) {
    lastMemoryCheck = millis();

    uint32_t freeHeap = ESP.getFreeHeap();
    uint16_t maxFreeBlockSize = ESP.getMaxFreeBlockSize();
    uint8_t heapFragmentation = 100 - (maxFreeBlockSize * 100) / freeHeap;
    
    Serial.print("Free heap: ");
    Serial.print(freeHeap);
    Serial.print(" bytes, Max block: ");
    Serial.print(maxFreeBlockSize);
    Serial.print(" bytes, Fragmentation: ");
    Serial.print(heapFragmentation);
    Serial.println("%");

    if (freeHeap < minFreeHeap) {
      Serial.println("Critical low memory detected, forcing restart...");
      pendingRestartReason = RESTART_MEMORY;
      forceRestart = true;
    }
    
    // Also check for severe fragmentation
    if (heapFragmentation > 85) {
      Serial.println("Severe heap fragmentation detected, forcing restart...");
      pendingRestartReason = RESTART_MEMORY;
      forceRestart = true;
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
    payload += "\"coffee_poured_total\":" + String(rtcData.coffeePouredTotal + coffeePouredCount) + ",";
    payload += "\"rssi\":" + String(rssi) + ",";
    payload += "\"free_heap\":" + String(freeHeap) + ",";
    payload += "\"uptime\":" + String(uptime) + ",";
    payload += "\"state\":" + String(stateValue) + ",";
    payload += "\"wifi_connected\":" + String(wifiConnected) + ",";
    payload += "\"mqtt_connected\":" + String(mqttConnected) + ",";
    payload += "\"wifi_reconnects\":" + String(wifiReconnectCount) + ",";
    payload += "\"mqtt_reconnects\":" + String(mqttReconnectCount) + ",";
    payload += "\"max_free_block\":" + String(ESP.getMaxFreeBlockSize()) + ",";
    payload += "\"last_restart_reason\":" + String(rtcData.lastRestartReason) + ",";
    // Restart metrics with reason labels
    payload += "\"restart_total\":" + String(rtcData.totalRestarts) + ",";
    payload += "\"restart_user\":" + String(rtcData.userRestarts) + ",";
    payload += "\"restart_watchdog\":" + String(rtcData.watchdogRestarts) + ",";
    payload += "\"restart_memory\":" + String(rtcData.memoryRestarts) + ",";
    payload += "\"restart_wifi\":" + String(rtcData.wifiRestarts) + ",";
    payload += "\"restart_mqtt\":" + String(rtcData.mqttRestarts) + ",";
    payload += "\"restart_health\":" + String(rtcData.healthRestarts) + ",";
    payload += "\"restart_unknown\":" + String(rtcData.unknownRestarts);
    payload += "}";

    if (!publishMetrics(payload)) {
      Serial.println("Metrics publishing failed, connection issues detected");
      systemHealthy = false;
      consecutiveFailures++;
    } else {
      systemHealthy = true;
      consecutiveFailures = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Smart Espresso Machine Starting...");

  // Initialize or read RTC data
  if (!readRTCData()) {
    Serial.println("Initializing RTC data");
    initRTCData();
  } else {
    Serial.println("RTC data loaded successfully");
    Serial.print("Total restarts: ");
    Serial.println(rtcData.totalRestarts);
    Serial.print("Last restart reason: ");
    Serial.println(rtcData.lastRestartReason);
    
    // Restore coffee count
    coffeePouredCount = 0; // Current session starts at 0
    Serial.print("Total coffee poured (all time): ");
    Serial.println(rtcData.coffeePouredTotal);
  }

  // Initialize pins
  pinMode(powerOn, OUTPUT);
  pinMode(pourDoubleCoffe, OUTPUT);
  digitalWrite(powerOn, HIGH);
  digitalWrite(pourDoubleCoffe, HIGH);

  // Setup watchdog timer (8 second timeout)
  ESP.wdtEnable(WDTO_8S);
  watchdog.attach(4, resetWatchdog); // Feed every 4 seconds

  // Setup health check timer
  healthCheck.attach(120, performHealthCheck); // Check every 2 minutes

  // Initialize timing variables
  bootTime = millis();
  lastAliveTime = millis();
  lastMemoryCheck = millis();
  lastWiFiCheck = millis();
  lastMQTTAttempt = 0;
  lastSuccessfulMetrics = millis();
  lastMQTTMessage = millis();

  // Initialize WiFi
  setupWiFi();

  // Setup MQTT with more conservative settings
  client.setServer(MQTT_BROKER, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(60); // Changed from 15 to 60 seconds - more standard
  client.setSocketTimeout(15); // 15 second socket timeout
  client.setBufferSize(512); // Increase buffer size for larger payloads

  Serial.println("Setup complete. System ready.");
  Serial.print("Free heap at startup: ");
  Serial.println(ESP.getFreeHeap());
}

void loop() {
  // Check for forced restart first
  if (forceRestart) {
    performRestart(pendingRestartReason);
  }

  // Feed the watchdog
  ESP.wdtFeed();

  // CRITICAL: Call MQTT loop frequently to maintain keep-alive
  if (client.connected()) {
    client.loop();
  }

  // Check WiFi connection periodically
  checkWiFiConnection();

  // Handle MQTT connection
  if (!client.connected() && WiFi.status() == WL_CONNECTED) {
    Serial.println("MQTT disconnected, attempting reconnection...");
    reconnectMQTT();
  }

  // MQTT loop again after potential reconnection
  if (client.connected()) {
    client.loop();
  }

  // Handle coffee brewing process
  handleCoffeeProcess();

  // Check for state timeouts
  checkStateTimeout();

  // Send periodic metrics
  sendPeriodicMetrics();

  // MQTT loop after metrics (which might take time)
  if (client.connected()) {
    client.loop();
  }

  // Check memory usage
  checkMemory();

  // Update RTC data periodically (every 10 minutes) to save coffee count
  static unsigned long lastRTCUpdate = 0;
  if (timeElapsed(lastRTCUpdate, 600000)) { // 10 minutes
    lastRTCUpdate = millis();
    rtcData.coffeePouredTotal += coffeePouredCount;
    coffeePouredCount = 0; // Reset current session count
    writeRTCData();
  }

  // MQTT loop one more time before delay
  if (client.connected()) {
    client.loop();
  }

  // Reduced delay to ensure more frequent MQTT loop calls
  delay(50);
}
