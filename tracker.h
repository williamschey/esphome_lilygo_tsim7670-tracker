#pragma once
//
// Helpers for the LilyGo T-SIM7670G-S3 tracker.
//
// All modem communication is done with plain AT commands over the modem UART
// (no PPP / IP stack). This keeps RAM and power low and lets the modem's own
// TLS + MQTT engine push positions while the ESP32 spends most of its time in
// deep sleep.
//
#include "esphome.h"
#include <string>
#include <vector>

namespace tracker {

using esphome::uart::UARTComponent;

// Drain any unread bytes from the modem RX buffer.
inline void uart_drain(UARTComponent *u) {
  uint8_t scratch[64];
  while (u->available() > 0) {
    size_t n = std::min<size_t>(64, (size_t) u->available());
    u->read_array(scratch, n);
  }
}

// Read from the UART until `token` is seen or `timeout_ms` elapses.
inline std::string wait_for(UARTComponent *u, const char *token, uint32_t timeout_ms) {
  std::string out;
  uint32_t start = esphome::millis();
  while (esphome::millis() - start < timeout_ms) {
    while (u->available() > 0) {
      uint8_t b;
      if (u->read_byte(&b)) out += static_cast<char>(b);
    }
    if (out.find(token) != std::string::npos) break;
    esphome::delay(10);
  }
  return out;
}

// Send an AT command and wait up to `timeout_ms` for OK/ERROR.
// Returns the full response text (may be empty if the modem hasn't booted).
inline std::string send_at(UARTComponent *u, const std::string &cmd, uint32_t timeout_ms) {
  uart_drain(u);
  std::string full = cmd + "\r\n";
  u->write_array(reinterpret_cast<const uint8_t *>(full.c_str()), full.size());
  std::string out;
  uint32_t start = esphome::millis();
  while (esphome::millis() - start < timeout_ms) {
    while (u->available() > 0) {
      uint8_t b;
      if (u->read_byte(&b)) out += static_cast<char>(b);
    }
    if (out.find("\r\nOK\r\n") != std::string::npos ||
        out.find("\r\nERROR") != std::string::npos) {
      break;
    }
    esphome::delay(10);
  }
  return out;
}

// Send an AT command that returns a '>' prompt, then stream `payload` bytes and
// wait for OK/ERROR. Used by the CMQTTTOPIC / CMQTTPAYLOAD commands.
inline std::string send_prompt(UARTComponent *u, const std::string &cmd,
                               const std::string &payload, uint32_t timeout_ms) {
  uart_drain(u);
  std::string full = cmd + "\r\n";
  u->write_array(reinterpret_cast<const uint8_t *>(full.c_str()), full.size());
  std::string pre = wait_for(u, ">", 5000);
  u->write_array(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.size());
  std::string out;
  uint32_t start = esphome::millis();
  while (esphome::millis() - start < timeout_ms) {
    while (u->available() > 0) {
      uint8_t b;
      if (u->read_byte(&b)) out += static_cast<char>(b);
    }
    if (out.find("\r\nOK\r\n") != std::string::npos ||
        out.find("\r\nERROR") != std::string::npos) {
      break;
    }
    esphome::delay(10);
  }
  return pre + out;
}

// Power up the GNSS engine once per wake cycle.
// GNSS works without a SIM. On the T-SIM7670G-S3 we (a) drive the modem's own
// GPIO4 HIGH to power the active antenna LNA (MODEM_GPS_ENABLE_GPIO in LilyGo's
// utilities.h — a SIM7670G internal GPIO, NOT an ESP32 pin), then (b) power the
// GNSS engine and wait for "+CGNSSPWR: READY!".
inline void gnss_power_on(UARTComponent *u, bool &powered_on) {
  if (powered_on) return;

  send_at(u, "AT", 500);
  send_at(u, "ATE0", 500);  // disable echo so responses are cleaner

  std::string dir = send_at(u, "AT+CGDRT=4,1", 1000);
  ESP_LOGI("GNSS", "CGDRT=4,1 (GPS LNA dir=out): '%s'", dir.c_str());
  std::string lvl = send_at(u, "AT+CGSETV=4,1", 1000);
  ESP_LOGI("GNSS", "CGSETV=4,1 (GPS LNA power on): '%s'", lvl.c_str());

  // CGNSSPWR=1 returns OK quickly, then emits "+CGNSSPWR: READY!" once the GNSS
  // subsystem has actually booted (can take ~20 s on a cold start).
  std::string pwr = send_at(u, "AT+CGNSSPWR=1", 30000);
  ESP_LOGI("GNSS", "CGNSSPWR=1: '%s'", pwr.c_str());
  if (pwr.find("OK") == std::string::npos) {
    ESP_LOGW("GNSS", "Modem not yet ready for GNSS power-on");
    return;
  }
  if (pwr.find("READY") == std::string::npos) {
    ESP_LOGW("GNSS", "GNSS engine OK but not READY yet; will keep fixing in background");
  }

  std::string mode = send_at(u, "AT+CGNSSMODE=15", 1000);  // all constellations
  ESP_LOGI("GNSS", "CGNSSMODE=15: '%s'", mode.c_str());
  powered_on = true;
  ESP_LOGI("GNSS", "GNSS engine powered on");
}

// Power the GNSS engine down to save battery before deep sleep. The modem radio
// is intentionally left attached to the network for a fast cellular publish.
inline void gnss_power_off(UARTComponent *u) {
  send_at(u, "AT+CGNSSPWR=0", 3000);
  send_at(u, "AT+CGSETV=4,0", 1000);  // turn the antenna LNA back off
  ESP_LOGI("GNSS", "GNSS engine powered off");
}

// Confirm the CA certificate referenced for TLS server verification is present
// in the modem's NVM (one-time provisioning — see README).
inline bool ca_present(UARTComponent *u, const char *cafile) {
  if (cafile == nullptr || cafile[0] == '\0') return true;
  std::string lst = send_at(u, "AT+CCERTLIST", 3000);
  if (lst.find(cafile) != std::string::npos) return true;
  ESP_LOGW("MQTT", "CA cert '%s' not on modem; server verification will fail. "
                   "Pre-provision it (see README) or set mqtt_away_authmode=0.", cafile);
  return false;
}

// Publish `payload` to `topic` on the away broker using the modem's native
// MQTT-over-TLS engine. Returns true on a successful publish.
//
// authmode: 0 = encrypt only (skip server cert verification),
//           1 = verify server cert against `cafile` (recommended).
inline bool publish_away(UARTComponent *u, const char *host, int port,
                         const char *client_id, const char *user, const char *pass,
                         const char *topic, const std::string &payload,
                         int authmode, const char *cafile) {
  ESP_LOGI("MQTT", "Cellular publish -> %s:%d topic=%s (%u bytes)",
           host, port, topic, (unsigned) payload.size());

  std::string creg = send_at(u, "AT+CGREG?", 3000);
  ESP_LOGD("MQTT", "CGREG? %s", creg.c_str());

  if (authmode >= 1) ca_present(u, cafile);

  // ---- SSL context 0 ----
  send_at(u, "AT+CSSLCFG=\"sslversion\",0,4", 2000);  // TLS 1.2
  char buf[160];
  snprintf(buf, sizeof(buf), "AT+CSSLCFG=\"authmode\",0,%d", authmode);
  send_at(u, buf, 2000);
  if (authmode >= 1 && cafile != nullptr && cafile[0] != '\0') {
    snprintf(buf, sizeof(buf), "AT+CSSLCFG=\"cacert\",0,\"%s\"", cafile);
    send_at(u, buf, 2000);
  }

  // ---- MQTT session ----
  send_at(u, "AT+CMQTTSTART", 5000);  // OK, or already started -> harmless ERROR
  snprintf(buf, sizeof(buf), "AT+CMQTTACCQ=0,\"%s\"", client_id);
  send_at(u, buf, 3000);
  send_at(u, "AT+CMQTTSSLCFG=0,0", 3000);  // bind SSL ctx 0 to MQTT client 0

  std::string conn = "AT+CMQTTCONNECT=0,\"ssl://" + std::string(host) + ":" +
                     std::to_string(port) + "\",60,1";
  if (user != nullptr && user[0] != '\0') {
    conn += ",\"" + std::string(user) + "\",\"" + std::string(pass) + "\"";
  }
  std::string cres = send_at(u, conn, 30000);
  cres += wait_for(u, "+CMQTTCONNECT:", 10000);  // async connect result URC
  ESP_LOGI("MQTT", "CONNECT: %s", cres.c_str());
  // A successful connect for client 0 reports "+CMQTTCONNECT: 0,0".
  bool connected = cres.find("+CMQTTCONNECT: 0,0") != std::string::npos;
  if (!connected) {
    ESP_LOGW("MQTT", "Cellular MQTT connect failed");
    send_at(u, "AT+CMQTTREL=0", 3000);
    send_at(u, "AT+CMQTTSTOP", 5000);
    return false;
  }

  // ---- topic + payload + publish (QoS 1) ----
  snprintf(buf, sizeof(buf), "AT+CMQTTTOPIC=0,%u", (unsigned) strlen(topic));
  send_prompt(u, buf, topic, 5000);
  snprintf(buf, sizeof(buf), "AT+CMQTTPAYLOAD=0,%u", (unsigned) payload.size());
  send_prompt(u, buf, payload, 5000);
  std::string pub = send_at(u, "AT+CMQTTPUB=0,1,60", 15000);
  ESP_LOGI("MQTT", "PUB: %s", pub.c_str());
  bool ok = pub.find("\r\nOK\r\n") != std::string::npos &&
            pub.find("\r\nERROR") == std::string::npos;

  send_at(u, "AT+CMQTTDISC=0,60", 10000);
  send_at(u, "AT+CMQTTREL=0", 3000);
  send_at(u, "AT+CMQTTSTOP", 5000);
  return ok;
}

}  // namespace tracker
