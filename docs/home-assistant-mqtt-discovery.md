# Home Assistant MQTT Discovery

TigerOS publishes MQTT discovery configuration when MQTT connects and Home Assistant Discovery is enabled.

## Requirements

- Home Assistant MQTT integration is installed and connected to the same broker as TigerOS.
- TigerOS MQTT is enabled in the Web Console.
- Discovery prefix is `homeassistant` unless changed in the Web Console.

## Web Console Setup

1. Log in to the TigerOS Web Console.
2. Open the MQTT section.
3. Enable MQTT and enter broker host, port, username, password, and client ID.
4. Enable Home Assistant Discovery.
5. Keep Discovery Prefix as `homeassistant` unless your Home Assistant MQTT integration uses a custom prefix.
6. Save MQTT settings.

TigerOS reconnects MQTT automatically and publishes retained discovery configuration.

![TigerOS MQTT settings example](/Users/bryanchen/Documents/New%20project/TigerOS/docs/images/home-assistant-mqtt-settings.svg)

## Discovery Topics

TigerOS publishes retained config messages to:

```text
homeassistant/sensor/{device_id}/rssi/config
homeassistant/sensor/{device_id}/heap/config
homeassistant/sensor/{device_id}/uptime/config
homeassistant/sensor/{device_id}/firmware/config
homeassistant/button/{device_id}/restart/config
homeassistant/switch/{device_id}/led/config
```

Runtime state is read from:

```text
tigeros/{device_id}/status
```

Commands are sent to:

```text
tigeros/{device_id}/command
```

## Entities

- Button: Restart Device
- Sensor: RSSI
- Sensor: Free Heap
- Sensor: Uptime
- Sensor: Firmware Version
- Switch: LED control placeholder

![Home Assistant device page example](/Users/bryanchen/Documents/New%20project/TigerOS/docs/images/home-assistant-device-page.svg)

## Test With Mosquitto

Watch discovery configs:

```bash
mosquitto_sub -h <broker> -t "homeassistant/#" -v
```

Watch TigerOS status:

```bash
mosquitto_sub -h <broker> -t "tigeros/+/status" -v
```

Trigger restart from the same command topic Home Assistant uses:

```bash
mosquitto_pub -h <broker> -t "tigeros/<device_id>/command" -m '{"command":"reboot"}'
```

Set the LED placeholder switch on:

```bash
mosquitto_pub -h <broker> -t "tigeros/<device_id>/command" -m '{"command":"control","target":"led","value":true}'
```

## Notes

- Discovery messages are retained so Home Assistant can rediscover the device after restart.
- Entity availability follows `value_json.online` from the status topic.
- The LED switch is a framework placeholder until board-specific GPIO mapping is added.
- MQTT broker security controls who can send Home Assistant commands.
