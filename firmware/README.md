# Firmware: MQTT & OTA Logic

The firmware is designed to operate in a low-power friendly manner by only maintaining a WiFi/MQTT connection during active telemetry bursts.

## Connection Lifecycle
1. **Wake**: Device wakes up every 15 minutes (or on event).
2. **Deinit Stacks**: BLE and ESP-NOW are paused to free up radio resources for WiFi.
3. **Connect**: Connects to the configured WiFi and MQTT broker.
4. **Uplink**: Publishes telemetry and crash logs.
5. **Listening Window**: The device stays connected for **5 seconds** after the last uplink. During this time, it calls `mqttHandler.loop()` frequently to process incoming commands.
6. **Command Handling**:
   - **Legacy**: Listens on `ae/device/<MAC>/command` for `check_fw` triggers.
   - **JIT/Push**: Subscribes to `ae/downlink/<MAC>/#`. OTA commands are received on `ae/downlink/<MAC>/OTA`.
7. **Cleanup**: Disconnects WiFi, restores ESP-NOW channel/peers, and resumes BLE advertising.

## Direct OTA (Push)
When an OTA command is received via the downlink topic, the `OtaHandler` parses the URL and metadata (version, MD5) and initiates `performUpdate()` immediately, bypassing the standard polling check.
