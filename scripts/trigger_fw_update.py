import argparse
import json
import sys
import time
try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Error: paho-mqtt library not found.")
    print("Please install it using: pip install paho-mqtt")
    sys.exit(1)

# Default Defaults (can be overridden by args)
DEFAULT_BROKER = "mqtt.aceselectronics.com.au"
DEFAULT_PORT = 1883
DEFAULT_USER = "aesmartshunt"
DEFAULT_PASS = "AERemoteAccess2024!"

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to MQTT Broker!")
    else:
        print(f"Failed to connect, return code {rc}")

def send_command(mac, cmd, broker, port, user, password):
    client = mqtt.Client(client_id=f"admin_trigger_{int(time.time())}")
    client.username_pw_set(user, password)
    client.on_connect = on_connect

    print(f"Connecting to {broker}:{port}...")
    try:
        client.connect(broker, port, 60)
    except Exception as e:
        print(f"Connection failed: {e}")
        return

    client.loop_start()
    time.sleep(1) # Wait for connect

    topic = f"ae/device/{mac}/command"
    payload = json.dumps({"cmd": cmd})

    print(f"Publishing to {topic}: {payload}")
    msg_info = client.publish(topic, payload, qos=1)
    msg_info.wait_for_publish()
    
    print("Command sent.")
    time.sleep(1) # Allow network flush
    client.loop_stop()
    client.disconnect()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Trigger Firmware Check via MQTT")
    parser.add_argument("mac", help="MAC Address of the device (e.g., AABBCCDDEEFF)")
    parser.add_argument("--cmd", default="check_fw", help="Command to send (default: check_fw)")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="MQTT Broker Address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="MQTT Broker Port")
    parser.add_argument("--user", default=DEFAULT_USER, help="MQTT Username")
    parser.add_argument("--password", default=DEFAULT_PASS, help="MQTT Password")

    args = parser.parse_args()
    
    # Normalize MAC
    mac = args.mac.replace(":", "").upper()
    
    send_command(mac, args.cmd, args.broker, args.port, args.user, args.password)
