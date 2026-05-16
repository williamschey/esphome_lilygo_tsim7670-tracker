# ESP Tracker — LilyGo T-SIM7670G-S3

ESPHome configuration for the [LilyGo T-SIM7670G-S3](https://lilygo.cc/products/t-sim-7670g-s3)
that tracks position with the on-board GNSS receiver and reports it to Home
Assistant over **either** WiFi **or** the SIM7670G's 4G LTE Cat-1 cellular
modem (whichever is available).

## How it works

* **WiFi** is configured normally and is the preferred path — the native
  ESPHome API (encrypted) works out of the box on the local LAN.
* **Cellular** uses the experimental `modem` component (PR
  [esphome/esphome#6721](https://github.com/esphome/esphome/pull/6721)) to
  bring up a PPP link over the modem UART. WiFi PR
  [#7147](https://github.com/esphome/esphome/pull/7147) lets the WiFi stack
  share the modem's network interface so both can coexist.
* **GPS** — the SIM7670 doesn't forward NMEA via URC, so the config polls
  `AT+CGNSSINFO` every 20 s, converts the response into `$GPGGA` / `$GPRMC`
  sentences, and feeds them through a `nulluart` into the stock ESPHome `gps`
  component. Latitude/longitude/altitude/speed/course/satellites/HDOP are then
  exposed as Home Assistant sensors and the `gps` time platform keeps the
  clock in sync.

> Cellular networks usually hand out a private (CGNAT) IP, so Home Assistant
> can't reach the device directly when only the modem is up. For
> always-online tracking add either a WireGuard tunnel back to your HA host
> or use the `mqtt:` component instead of (or in addition to) `api:`.

## Pinout used (LilyGo T-SIM7670G-S3)

| Function     | ESP32-S3 GPIO | Notes                                  |
|--------------|---------------|----------------------------------------|
| Modem RX     | GPIO11        | Connected to modem **TX**              |
| Modem TX     | GPIO10        | Connected to modem **RX**              |
| Modem PWRKEY | GPIO18        | Pulse high ~100 ms to power on         |
| Modem RESET  | GPIO17        | Active LOW                             |
| Status LED   | GPIO12        |                                        |
| Battery ADC  | GPIO4         | 1:2 divider on board                   |

Source: [LilyGo `utilities.h`](https://github.com/Xinyuan-LilyGO/LilyGO-T-SIM-A76XX/blob/main/examples/Arduino_Basic/utilities.h).

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in your credentials.
2. Set the cellular `apn:` (and `sim_pin:` if needed) at the top of
   `esp-tracker.yaml`.
3. Compile & flash:
   ```bash
   esphome run esp-tracker.yaml
   ```
4. Add the device in Home Assistant — the GPS entities show up as
   `sensor.latitude`, `sensor.longitude`, etc. To turn them into a
   `device_tracker`, add a [template tracker](https://www.home-assistant.io/integrations/device_tracker.template/)
   pointing at those sensors, or use the
   [`mqtt_room`/`mqtt` device tracker](https://www.home-assistant.io/integrations/device_tracker.mqtt/)
   when running over cellular.

## Notes

* `external_components:` pulls in the modem and WiFi-NAT PRs and the
  `nulluart` helper from `oarcher/piotech`. Once those PRs are merged this
  block can be dropped.
* The SIM7670 firmware sometimes returns 17 instead of 18 fields from
  `AT+CGNSSINFO`; the lambda handles both shapes.
* The modem draws current spikes >2 A — power the board over USB-C with a
  capable supply or via the on-board LiPo connector.
