#pragma once

// Messages Concha sends outside the DAW, at a given time, from one process-wide thread:
//  - MQTT (Strudel's .mqtt() / .robot()): ws:// and wss:// as Paho in the browser (MQTT 3.1.1 over a
//    WebSocket, subprotocol "mqtt"), and mqtt:// / mqtts:// (plain MQTT over TCP / TLS). The TCP and
//    TLS connection is libcurl's (CONNECT_ONLY); the WebSocket and MQTT packets are written here.
//    QoS 0, as Paho's send(). A connection opens on its first message; messages sent while it is
//    not up are dropped (as Strudel's `if (!cx.isConnected()) return`), retried every 5 s.
//  - serial ports (Strudel's .serial()): raw 8N1 at the given baud rate (POSIX termios; other
//    systems: phase 9).

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace concha::io {

using Clock = std::chrono::steady_clock;

struct MqttMessage {
    std::string host;   // ws://, wss://, mqtt://, mqtts:// URL
    std::string client; // client id
    std::string user, password;
    std::string topic, payload;
};

void sendMqtt(MqttMessage message, Clock::time_point due);
// port: a device path ("/dev/ttyACM0") or "default" (the first /dev/ttyACM*, then /dev/ttyUSB*)
void sendSerial(std::string port, int baud, std::vector<uint8_t> bytes, Clock::time_point due);
// What happened (connections, errors), for the console.
std::vector<std::string> takeMessages();

// For tests: MQTT packets as written on the wire (without the WebSocket framing).
std::vector<uint8_t> mqttConnectPacket(const std::string& client, const std::string& user, const std::string& password);
std::vector<uint8_t> mqttPublishPacket(const std::string& topic, const std::string& payload);
// A client WebSocket frame (binary, masked with `mask`).
std::vector<uint8_t> webSocketFrame(const std::vector<uint8_t>& payload, const uint8_t mask[4]);

} // namespace concha::io
