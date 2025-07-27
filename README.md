# Smart Espresso Machine Controller

An ESP8266-based IoT controller that automates espresso machine operation and provides comprehensive monitoring through MQTT and Prometheus metrics.

## Features

- **Remote Coffee Brewing**: Start coffee brewing via MQTT commands
- **Comprehensive Monitoring**: Real-time metrics including WiFi signal, memory usage, uptime, and connection status
- **Prometheus Integration**: Ready-to-use metrics for monitoring and alerting
- **Reliable 24/7 Operation**: Built-in watchdog, memory monitoring, and automatic recovery mechanisms
- **State Management**: Track brewing process from idle to pouring completion
- **Connection Resilience**: Automatic WiFi and MQTT reconnection with fallback mechanisms

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
   ```bash
   git clone <repository-url>
   cd smart-espresso
   ```

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

### Metrics Published

Published every 60 seconds to `metrics/cafeteira`:

```json
{
  "alive": 1,
  "coffee_poured": 5,
  "rssi": -45,
  "free_heap": 25000,
  "uptime": 3600,
  "state": 0,
  "wifi_connected": 1,
  "mqtt_connected": 1
}
```

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

  - prom_name: cafeteira_coffee_poured_total
    mqtt_name: "coffee_poured"
    help: "Total number of times coffee was poured"
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
```

## Reliability Features

### Watchdog Timer
- 30-second watchdog timer prevents system hangs
- Automatic restart if system becomes unresponsive

### Memory Management
- Monitors free heap memory every 5 minutes
- Automatic restart if memory drops below 1KB

### Connection Recovery
- Non-blocking WiFi reconnection with 30-second timeout
- MQTT reconnection with exponential backoff
- Automatic restart after 5 consecutive WiFi failures

### State Protection
- 5-minute timeout for any non-IDLE state
- Prevents system from getting stuck in brewing states

### Timing Safety
- Handles millis() overflow (49-day rollover)
- All timing operations are overflow-safe

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
const unsigned long coffeeWaitTime = 150000;      // Brewing wait time (2.5 minutes)
const unsigned long stateTimeout = 300000;        // Max state duration (5 minutes)
const unsigned long wifiTimeout = 30000;          // WiFi connection timeout
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
```

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
