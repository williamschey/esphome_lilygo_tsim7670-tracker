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
* **Deep sleep** — between reports the ESP32 deep-sleeps for `report_interval`.
  The GNSS engine is powered down; the modem radio is intentionally left
  attached to the network so the next cellular publish is fast.

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
   * `report_interval` — deep-sleep time between reports (default `5min`).
   * `mqtt_topic` — the topic both brokers publish to.
3. (Cellular TLS) provision the broker CA on the modem — see below.
4. Compile & flash:
   ```bash
   esphome run esp-tracker.yaml
   ```

## Home Assistant — device_tracker

Add an MQTT device tracker that reads the published JSON attributes:

```yaml
# configuration.yaml
mqtt:
  device_tracker:
    - name: "ESP Tracker 1"
      state_topic: "tracker/esp-tracker-1/location"
      value_template: "home"          # presence is derived from coordinates by zones
      json_attributes_topic: "tracker/esp-tracker-1/location"
```

The `latitude`, `longitude` and `gps_accuracy` attributes drive the map and
zone-based presence. Make sure `state_topic`/`json_attributes_topic` match
`mqtt_topic` in `esp-tracker.yaml`.

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

## Battery tuning

The biggest lever is `report_interval` (longer sleep = longer battery). Further
options:

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
