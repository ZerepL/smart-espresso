# Smart Espresso Machine Controller

An ESP8266-based IoT controller that automates espresso machine operation and provides comprehensive monitoring through MQTT and Prometheus metrics.

## Features

- **Remote Coffee Brewing**: Start coffee brewing via MQTT commands with enhanced state management
- **Comprehensive Monitoring**: Real-time metrics including WiFi signal, memory usage, uptime, connection status, and detailed restart tracking
- **Prometheus Integration**: Ready-to-use metrics for monitoring and alerting with expanded metric set
- **Ultra-Reliable 24/7 Operation**: Multi-layered watchdog system, memory monitoring, health checks, and automatic recovery mechanisms
- **Persistent State Management**: RTC memory storage for restart tracking and coffee count persistence across reboots
- **Advanced Connection Resilience**: Automatic WiFi and MQTT reconnection with exponential backoff, connection health monitoring, and forced recovery
- **Detailed Restart Analytics**: Track and categorize all restart reasons with persistent counters
- **Enhanced Health Monitoring**: Proactive system health checks with configurable thresholds and automatic recovery

## Hardware Requirements

- ESP-01 module (ESP8266)
- Two-way relay shield
- AMS1117 voltage regulator (5V to 3.3V)
- Andersson ESM 1.0 espresso machine (or compatible)

### Hardware Setup

The system is designed to interface directly with the Andersson ESM 1.0 espresso machine:

- **Power Source**: 5V output from the espresso machine's front button panel
- **Voltage Regulation**: AMS1117 converts 5V to 3.3V for ESP-01
- **Relay Power**: Direct 5V from espresso machine to relay shield
- **Control Interface**: Two-way relay shield connected to machine's control circuits

### Pin Configuration

- **Pin 0**: Power On control (espresso machine main power)
- **Pin 2**: Pour Double Coffee control (brewing trigger)

## Software Dependencies

- ESP8266WiFi library
- PubSubClient library (MQTT)
- Ticker library (watchdog timer)

## Installation

1. **Clone the repository**:

2. **Configure credentials**:
   Edit `arduino-secrets.h` with your network settings:
   ```cpp
   const char* WIFI_PASSWORD = "your_wifi_password";
   const char* WIFI_NAME = "your_wifi_ssid";
   const char* MQTT_BROKER = "your_mqtt_broker_ip";
   ```

3. **Upload to ESP-01**:
   - Open `smart-espresso.ino` in Arduino IDE
   - Select "Generic ESP8266 Module" as board
   - Configure for ESP-01 (1MB flash, no SPIFFS)
   - Upload the code using appropriate programmer

## MQTT Interface

### Topics

- **Command Topic**: `cafeteira/start`
- **Metrics Topic**: `metrics/cafeteira`

### Commands

Send these messages to `cafeteira/start`:

- `"on"` - Start coffee brewing cycle
- `"reset"` - Reset state and coffee counter
- `"restart"` - Restart the ESP8266 device
- `"ping"` - Health check command to verify system responsiveness

### Metrics Published

Published every 60 seconds to `metrics/cafeteira`:

```json
{
  "alive": 1,
  "coffee_poured": 5,
  "coffee_poured_total": 127,
  "rssi": -45,
  "free_heap": 25000,
  "uptime": 3600,
  "state": 0,
  "wifi_connected": 1,
  "mqtt_connected": 1,
  "wifi_reconnects": 3,
  "mqtt_reconnects": 7,
  "max_free_block": 24000,
  "last_restart_reason": 1,
  "restart_total": 15,
  "restart_user": 2,
  "restart_watchdog": 1,
  "restart_memory": 0,
  "restart_wifi": 3,
  "restart_mqtt": 2,
  "restart_health": 1,
  "restart_unknown": 6
}
```

#### Metric Descriptions

- **alive**: Device heartbeat (always 1 when publishing)
- **coffee_poured**: Coffee count for current session (resets on restart)
- **coffee_poured_total**: Total coffee count across all sessions (persistent)
- **rssi**: WiFi signal strength in dBm
- **free_heap**: Available memory in bytes
- **uptime**: Seconds since last restart
- **state**: Current brewing state (IDLE=0, WAITING=1, POURING=2)
- **wifi_connected**: WiFi connection status (1=connected, 0=disconnected)
- **mqtt_connected**: MQTT connection status (1=connected, 0=disconnected)
- **wifi_reconnects**: WiFi reconnection attempts since boot
- **mqtt_reconnects**: MQTT reconnection attempts since boot
- **max_free_block**: Largest contiguous memory block available
- **last_restart_reason**: Code for the last restart cause
- **restart_total**: Total number of restarts (persistent)
- **restart_user**: User-initiated restarts (persistent)
- **restart_watchdog**: Watchdog-triggered restarts (persistent)
- **restart_memory**: Memory-related restarts (persistent)
- **restart_wifi**: WiFi-related restarts (persistent)
- **restart_mqtt**: MQTT-related restarts (persistent)
- **restart_health**: Health check-triggered restarts (persistent)
- **restart_unknown**: Restarts with unknown cause (persistent)

## State Machine

The system operates in three states:

- **IDLE (0)**: Ready for commands
- **WAITING (1)**: Machine heating up (2.5 minutes)
- **POURING (2)**: Actively dispensing coffee (~700ms)

### State Flow
```
IDLE → [MQTT "on" command] → WAITING → [2.5 min timer] → POURING → IDLE
```

## Prometheus Integration

### Setup mqtt2prometheus

Use the following configuration for mqtt2prometheus v0.1.7:

```yaml
mqtt:
  server: tcp://mosquitto:1883
  topic_path: metrics/+
  device_id_regex: "metrics/(?P<deviceid>[^/]+)"
  qos: 0

cache:
  timeout: 24h

metrics:
  - prom_name: device_alive
    mqtt_name: "alive"
    help: "Whether the device is alive (1) or not (0)"
    type: gauge

  - prom_name: device_wifi_rssi_dbm
    mqtt_name: "rssi"
    help: "WiFi signal strength (RSSI) in dBm"
    type: gauge

  - prom_name: cafeteira_coffee_poured_session_total
    mqtt_name: "coffee_poured"
    help: "Number of coffees poured in current session"
    type: counter

  - prom_name: cafeteira_coffee_poured_total
    mqtt_name: "coffee_poured_total"
    help: "Total number of coffees poured across all sessions"
    type: counter

  - prom_name: device_free_heap_bytes
    mqtt_name: "free_heap"
    help: "Amount of free heap memory in bytes"
    type: gauge

  - prom_name: device_uptime_seconds_total
    mqtt_name: "uptime"
    help: "Device uptime in seconds since last restart"
    type: counter

  - prom_name: cafeteira_state_info
    mqtt_name: "state"
    help: "Current state of the coffee machine (IDLE=0, WAITING=1, POURING=2)"
    type: gauge

  - prom_name: device_wifi_connected
    mqtt_name: "wifi_connected"
    help: "WiFi connection status (1=connected, 0=disconnected)"
    type: gauge

  - prom_name: device_mqtt_connected
    mqtt_name: "mqtt_connected"
    help: "MQTT connection status (1=connected, 0=disconnected)"
    type: gauge

  - prom_name: device_wifi_reconnects_total
    mqtt_name: "wifi_reconnects"
    help: "Number of WiFi reconnection attempts since boot"
    type: counter

  - prom_name: device_mqtt_reconnects_total
    mqtt_name: "mqtt_reconnects"
    help: "Number of MQTT reconnection attempts since boot"
    type: counter

  - prom_name: device_max_free_block_bytes
    mqtt_name: "max_free_block"
    help: "Size of largest contiguous memory block available"
    type: gauge

  - prom_name: device_last_restart_reason
    mqtt_name: "last_restart_reason"
    help: "Code indicating the reason for the last restart"
    type: gauge

  - prom_name: device_restart_total
    mqtt_name: "restart_total"
    help: "Total number of device restarts"
    type: counter

  - prom_name: device_restart_user_total
    mqtt_name: "restart_user"
    help: "Number of user-initiated restarts"
    type: counter

  - prom_name: device_restart_watchdog_total
    mqtt_name: "restart_watchdog"
    help: "Number of watchdog-triggered restarts"
    type: counter

  - prom_name: device_restart_memory_total
    mqtt_name: "restart_memory"
    help: "Number of memory-related restarts"
    type: counter

  - prom_name: device_restart_wifi_total
    mqtt_name: "restart_wifi"
    help: "Number of WiFi-related restarts"
    type: counter

  - prom_name: device_restart_mqtt_total
    mqtt_name: "restart_mqtt"
    help: "Number of MQTT-related restarts"
    type: counter

  - prom_name: device_restart_health_total
    mqtt_name: "restart_health"
    help: "Number of health check-triggered restarts"
    type: counter

  - prom_name: device_restart_unknown_total
    mqtt_name: "restart_unknown"
    help: "Number of restarts with unknown cause"
    type: counter
```

### Example Prometheus Queries

```promql
# Check if coffee machine is brewing
cafeteira_state_info{deviceid="cafeteira"} > 0

# Monitor connection health
device_wifi_connected{deviceid="cafeteira"} * device_mqtt_connected{deviceid="cafeteira"}

# Coffee production rate (cups per hour)
rate(cafeteira_coffee_poured_total{deviceid="cafeteira"}[1h]) * 3600

# Memory usage monitoring
device_free_heap_bytes{deviceid="cafeteira"}

# Device uptime
device_uptime_seconds_total{deviceid="cafeteira"}

# Memory fragmentation monitoring
(device_free_heap_bytes{deviceid="cafeteira"} - device_max_free_block_bytes{deviceid="cafeteira"}) / device_free_heap_bytes{deviceid="cafeteira"} * 100

# Restart frequency by type
rate(device_restart_total{deviceid="cafeteira"}[24h]) * 86400

# Connection stability (reconnection rate)
rate(device_wifi_reconnects_total{deviceid="cafeteira"}[1h]) + rate(device_mqtt_reconnects_total{deviceid="cafeteira"}[1h])

# System health score (0-1, where 1 is perfect health)
device_wifi_connected{deviceid="cafeteira"} * device_mqtt_connected{deviceid="cafeteira"} * (device_free_heap_bytes{deviceid="cafeteira"} > 5000)
```

## Reliability Features

### Multi-Layer Watchdog System
- **Hardware Watchdog**: 8-second hardware watchdog timer prevents system hangs
- **Software Watchdog**: 4-second software watchdog feeding ensures system responsiveness
- **Automatic restart** if system becomes unresponsive

### Advanced Memory Management
- **Continuous monitoring**: Checks free heap memory every 5 minutes
- **Fragmentation detection**: Monitors heap fragmentation levels
- **Critical thresholds**: Automatic restart if memory drops below 1KB or fragmentation exceeds 85%
- **Memory leak protection**: Prevents gradual memory degradation

### Enhanced Connection Recovery
- **Non-blocking WiFi reconnection** with 30-second timeout
- **MQTT reconnection** with exponential backoff and detailed error reporting
- **Connection health monitoring**: Tracks reconnection attempts and forces restart after excessive failures
- **Automatic restart** after 5 consecutive WiFi failures or 50 MQTT failures
- **Daily counter reset** for long-term stability

### Comprehensive Health Monitoring
- **Periodic health checks** every 2 minutes
- **Metrics silence detection**: Restart if no successful metrics published for 5 minutes
- **MQTT activity monitoring**: Restart if no MQTT activity for 10 minutes
- **Connection quality assessment**: Proactive detection of degraded connections

### Persistent State Management
- **RTC memory storage**: Maintains restart counters and coffee counts across power cycles
- **CRC32 validation**: Ensures data integrity in RTC memory
- **Detailed restart categorization**: Tracks 8 different restart reasons
- **Coffee count persistence**: Total coffee count survives restarts and power outages

### State Protection
- **5-minute timeout** for any non-IDLE state prevents stuck states
- **State validation**: Ensures brewing process completes properly
- **Automatic recovery** from invalid states

### Timing Safety
- **Overflow-safe timing**: Handles millis() overflow (49-day rollover)
- **All timing operations** are overflow-resistant
- **Consistent time tracking** across long-term operation

## Enhanced Resilience Features

### Advanced MQTT Management
- **Optimized MQTT loop calls**: Multiple strategic `client.loop()` calls throughout the main loop ensure reliable message processing
- **Enhanced connection testing**: Automatic test publish on connection to verify MQTT broker communication
- **Improved buffer management**: Increased buffer size (512 bytes) and socket timeout (15 seconds) for better reliability
- **Clean session handling**: Proper disconnect/reconnect sequence with unique client IDs to prevent conflicts

### Proactive Health Monitoring
- **Multi-metric health assessment**: Monitors metrics publishing success, MQTT activity, and connection quality
- **Automatic recovery triggers**: System automatically restarts when health thresholds are exceeded
- **Connection attempt tracking**: Detailed monitoring of WiFi (>20 attempts) and MQTT (>50 attempts) reconnection failures
- **Daily counter reset**: Prevents false positives during long-term operation by resetting counters every 24 hours

### Enhanced Memory Protection
- **Heap fragmentation monitoring**: Tracks memory fragmentation percentage and triggers restart at >85% fragmentation
- **Dual memory thresholds**: Monitors both total free heap and largest contiguous block
- **Proactive memory management**: Regular memory checks prevent gradual degradation

### Robust State Management
- **Periodic RTC updates**: Coffee count and restart data saved to RTC memory every 10 minutes
- **Session-based counting**: Separates current session coffee count from persistent total count
- **Data integrity validation**: CRC32 checksums ensure RTC data reliability across power cycles

### Connection Quality Assurance
- **Enhanced WiFi validation**: Checks both connection status and IP address validity
- **MQTT state monitoring**: Detailed MQTT connection state reporting with error code translation
- **Publish retry mechanism**: Multiple attempts (up to 3) for critical metrics publishing
- **Connection health scoring**: Comprehensive assessment of overall system connectivity

## Monitoring and Debugging

### Serial Output
Connect via USB to monitor system status:
- Connection events
- State transitions
- Memory usage
- Error conditions

### MQTT Monitoring
Subscribe to `metrics/cafeteira` to monitor:
- Device health
- Connection status
- Coffee production statistics
- System performance

## Configuration Options

### Timing Constants
```cpp
const unsigned long aliveInterval = 60000;        // Metrics interval (1 minute)
const unsigned long memoryCheckInterval = 300000; // Memory check interval (5 minutes)
const unsigned long wifiCheckInterval = 30000;    // WiFi check interval (30 seconds)
const unsigned long mqttRetryInterval = 5000;     // MQTT retry interval (5 seconds)
const unsigned long coffeeWaitTime = 150000;      // Brewing wait time (2.5 minutes)
const unsigned long stateTimeout = 300000;        // Max state duration (5 minutes)
const unsigned long wifiTimeout = 30000;          // WiFi connection timeout
const unsigned long healthCheckInterval = 120000; // Health check interval (2 minutes)
const unsigned long maxSilentTime = 300000;       // Max time without metrics (5 minutes)
const unsigned long maxMQTTSilentTime = 600000;   // Max time without MQTT activity (10 minutes)
const int minFreeHeap = 1000;                     // Minimum memory before restart
```

### Pin Configuration
```cpp
const int powerOn = 0;           // Power control pin
const int pourDoubleCoffe = 2;   // Pour control pin
```

## Troubleshooting

### Common Issues

1. **Device not connecting to WiFi**
   - Check credentials in `arduino-secrets.h`
   - Verify WiFi network is 2.4GHz (ESP8266 limitation)
   - Monitor serial output for connection attempts

2. **MQTT messages not received**
   - Verify MQTT broker is accessible
   - Check topic names match configuration
   - Monitor MQTT broker logs

3. **Device restarting frequently**
   - Check power supply stability from espresso machine
   - Monitor memory usage via metrics
   - Review serial output for error patterns
   - Verify AMS1117 voltage regulation is stable

4. **Coffee not brewing**
   - Verify relay connections to espresso machine controls
   - Check relay shield power from machine's 5V output
   - Test manual relay operation
   - Ensure proper timing for Andersson ESM 1.0

### Debug Commands

```bash
# Monitor MQTT messages
mosquitto_sub -h your_broker -t "metrics/cafeteira"

# Send test commands
mosquitto_pub -h your_broker -t "cafeteira/start" -m "on"
mosquitto_pub -h your_broker -t "cafeteira/start" -m "ping"
mosquitto_pub -h your_broker -t "cafeteira/start" -m "restart"

# Monitor all MQTT traffic for debugging
mosquitto_sub -h your_broker -t "#" -v
```

### Advanced Monitoring

The enhanced system provides detailed restart analytics and connection monitoring:

```bash
# Example metrics output showing restart tracking
{
  "restart_total": 15,
  "restart_user": 2,
  "restart_watchdog": 1,
  "restart_memory": 0,
  "restart_wifi": 3,
  "restart_mqtt": 2,
  "restart_health": 1,
  "restart_unknown": 6,
  "last_restart_reason": 1
}
```

### Restart Reason Codes
- **0**: Unknown restart
- **1**: User-initiated restart (via MQTT command)
- **2**: Watchdog timeout
- **3**: Memory exhaustion or fragmentation
- **4**: WiFi connection failures
- **5**: MQTT connection failures
- **6**: Health check failure
- **7**: Power-on/first boot

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## Safety Notice

⚠️ **Important**: This project involves interfacing with electrical appliances. Always work with caution, disconnect power when making connections, and verify all wiring before powering on. The authors are not responsible for any accidents or damage - use at your own risk and ensure you have the necessary skills to work safely with electrical systems.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
