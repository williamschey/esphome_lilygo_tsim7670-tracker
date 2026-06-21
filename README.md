# ESP Tracker — LilyGo T-SIM7670G-S3

ESPHome configuration for the [LilyGo T-SIM7670G-S3](https://lilygo.cc/products/t-sim-7670g-s3)
that tracks position with the on-board GNSS receiver and **pushes** it to Home
Assistant over MQTT, using WiFi at home and 4G LTE Cat-1 cellular when away —
optimised for **security, reliability and battery life**.

## How it works

The device runs a one-shot cycle and then deep-sleeps:

```
wake → power GNSS → wait for a fix → publish position → power GNSS down → deep sleep
```

* **GNSS** — the SIM7670 doesn't forward NMEA via URC, so the config polls
  `AT+CGNSSINFO`, parses the latitude/longitude/altitude/speed/course/satellites
  and publishes them to local sensors **and** to MQTT as a JSON payload.
* **Home (WiFi/LAN)** — when WiFi is up, the position is published with
  ESPHome's built-in `mqtt:` client to your **home broker**. The native ESPHome
  API is also available on the LAN for debugging/OTA.
* **Away (cellular)** — when WiFi is *not* available, the position is published
  to a separate **away broker** using the **modem's own MQTT-over-TLS engine**
  (`AT+CMQTT*` / `AT+CSSLCFG`). No PPP/IP stack is brought up on the ESP32, which
  keeps RAM and power low and works through carrier CGNAT (the device *pushes*,
  so Home Assistant never has to reach it).
* **Deep sleep** — between reports the ESP32 deep-sleeps with an adaptive
  cadence: `report_interval_near` (default `10s`) when the last fix was within
  `near_home_radius_m` (default `1000` m) of home, and `report_interval_far`
  (default `30s`) otherwise. The GNSS engine is powered down; the modem radio is
  intentionally left attached to the network so the next cellular publish is fast.

> **No custom Home Assistant integration is required.** You only need an MQTT
> broker (e.g. the Mosquitto add-on) and HA's built-in MQTT integration. The
> device is surfaced as a `device_tracker` (see below).

## Architecture at a glance

| Path  | Transport                | Broker        | Encryption           |
|-------|--------------------------|---------------|----------------------|
| Home  | ESPHome `mqtt:` over WiFi | home broker   | WPA2 (+ optional TLS)|
| Away  | modem `AT+CMQTT` over LTE | away broker   | TLS (modem engine)   |

Both paths publish the **same JSON payload** to the **same topic**
(`mqtt_topic`), so Home Assistant sees one entity regardless of which network
the device used:

```json
{"latitude":-33.8688,"longitude":151.2093,"gps_accuracy":5.0,
 "altitude":30.0,"speed":0.0,"course":0.0,"satellites":9,"battery":3.95}
```

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

## Files

| File                   | Purpose                                                      |
|------------------------|--------------------------------------------------------------|
| `esp-tracker.yaml`     | Main ESPHome config (deep-sleep cycle, WiFi/cellular publish)|
| `tracker.h`            | C++ helpers: AT command I/O, GNSS power, modem MQTT-over-TLS |
| `secrets.yaml`         | WiFi / API / OTA / **home** broker credentials (git-ignored) |
| `cellular.yaml`        | **Away** broker credentials as substitutions (git-ignored)   |

`secrets.yaml` and `cellular.yaml` are git-ignored. The away-broker values live
in `cellular.yaml` (not `secrets.yaml`) because they are injected into an
AT-command lambda, and ESPHome cannot expand `!secret` inside a lambda — a
`packages:` include of substitutions is used instead.

## Setup

1. Copy the example files and fill them in:
   ```bash
   cp secrets.yaml.example  secrets.yaml
   cp cellular.yaml.example cellular.yaml
   ```
   * `secrets.yaml` — WiFi, API key (`openssl rand -base64 32`), OTA password,
     and your **home** broker host/user/password.
   * `cellular.yaml` — your **away** broker host/port/client-id/user/password.
2. Adjust the tunables at the top of `esp-tracker.yaml`:
   * `report_interval_far` / `report_interval_near` — deep-sleep time between
     reports when away from / near home (defaults `30s` / `10s`).
   * `home_latitude` / `home_longitude` — your home coordinates.
   * `near_home_radius_m` — distance from home within which the faster
     `report_interval_near` cadence is used (default `1000` m).
   * `mqtt_topic` — the topic both brokers publish to.
3. (Cellular TLS) provision the broker CA on the modem — see below.
4. Compile & flash:
   ```bash
   esphome run esp-tracker.yaml
   ```

## Home Assistant — device_tracker

The published JSON already contains `latitude`, `longitude` and `gps_accuracy`,
so the simplest match is the **`mqtt_json`** device tracker, which parses those
fields, places the device on the map and lets Home Assistant **derive
`home`/zone presence from the coordinates** automatically:

```yaml
# configuration.yaml
device_tracker:
  - platform: mqtt_json
    devices:
      esp_tracker_1: "tracker/esp-tracker-1/location"
```

This reads `latitude`, `longitude`, `gps_accuracy` and `battery`, sets
`source_type: gps`, and creates `device_tracker.esp_tracker_1`. Make sure the
topic matches `mqtt_topic` in `esp-tracker.yaml`.

Alternatively, use the `mqtt:` device tracker, which exposes the payload as
entity attributes (note: this variant does **not** auto-derive zones —
`value_template` sets the state, the JSON only populates attributes):

```yaml
# configuration.yaml
mqtt:
  device_tracker:
    - name: "ESP Tracker 1"
      state_topic: "tracker/esp-tracker-1/location"
      value_template: "home"
      json_attributes_topic: "tracker/esp-tracker-1/location"
```

Either way, make sure `devices`/`state_topic`/`json_attributes_topic` match
`mqtt_topic` in `esp-tracker.yaml`. ESPHome MQTT discovery is disabled
(`discovery: false`), so this manual config is required.

## TLS provisioning for the away (cellular) broker

The modem performs the TLS handshake itself, so the broker's CA certificate
must be stored in the modem's NVM **once** (it survives reflashes). With a
serial terminal on the modem UART (or a temporary ESPHome AT passthrough):

```
AT+CCERTDOWN="cacert.pem",<size-in-bytes>
> <paste the raw PEM bytes>
```

Then in `cellular.yaml` keep:

```yaml
mqtt_away_authmode: "1"          # verify the broker certificate (recommended)
mqtt_away_cafile:   "cacert.pem"
```

If you just want encryption without server verification while testing, set
`mqtt_away_authmode: "0"` (the connection is still TLS, but the broker cert is
not validated). The config logs a warning if `authmode 1` is set but the CA file
isn't present on the modem.

For mutual TLS, also upload `clientcert.pem` / `clientkey.pem` with `AT+CCERTDOWN`
and bind them via `AT+CSSLCFG="clientcert"/"clientkey"` (extend `publish_away()`
in `tracker.h`).

## Adaptive reporting cadence (near home)

The deep-sleep interval adapts to how far the last fix was from home:

```
fix within near_home_radius_m of home → sleep report_interval_near (default 10s)
fix farther than near_home_radius_m    → sleep report_interval_far  (default 30s)
```

* Each wake the device computes the great-circle (haversine) distance from the
  fix to `(home_latitude, home_longitude)`. If it is within `near_home_radius_m`
  (default `1000` m) the next deep sleep uses the faster `report_interval_near`
  cadence; otherwise it uses `report_interval_far`.
* The distance is evaluated on every cycle, so the device automatically speeds up
  as it approaches home and slows down again as it leaves — no external pin or
  wiring is required.
* This is independent of the 100 m `home_radius_m` geofence used for the
  "Located at Home" presence diagnostic and the `home` field in the MQTT payload.

Tunables (top of `esp-tracker.yaml`):

```yaml
report_interval_far: 30s     # deep-sleep cadence when away from home
report_interval_near: 10s    # deep-sleep cadence within near_home_radius_m
near_home_radius_m: "1000"   # radius (metres) selecting the faster cadence
```

The **"Stay Awake (maintenance)"** switch suspends deep sleep entirely over the
API (while on WiFi) for OTA updates and debugging.

## Battery tuning

The biggest lever is the report cadence — raising `report_interval_far` /
`report_interval_near` (longer sleep = longer battery). Further options:

* **Keep the modem attached** (current behaviour) for fast re-publish, or for
  the lowest idle draw enable LTE **PSM/eDRX**, or fully power the modem off
  between cycles (re-attaching costs time + energy each wake).
* **AGPS / hot start** — a cold GNSS fix can take 30–60 s. Keeping the GNSS
  almanac/ephemeris cached (or enabling AGPS) makes re-fixes near-instant and is
  the single biggest in-cycle energy saver.
* Toggle the **"Stay Awake (maintenance)"** switch over the API (while on WiFi)
  to suspend deep sleep for OTA updates and debugging.

## Notes

* The cellular MQTT path uses the SIMCom A7670/SIM7670 `AT+CMQTT*` command set;
  see the [SIM7670 series AT manual](https://simcom.ee/documents/SIM7670G/).
* The SIM7670 firmware sometimes returns 17 instead of 18 fields from
  `AT+CGNSSINFO`; the parser handles both shapes.
* GNSS works without a SIM. To get a fix the config drives the modem's internal
  GPIO4 high to power the active antenna LNA (`AT+CGDRT`/`AT+CGSETV`), then powers
  the GNSS engine (`AT+CGNSSPWR=1`).
* The modem draws current spikes >2 A — power the board over USB-C with a
  capable supply or via the on-board LiPo connector.
