// Minimal ESP-IDF native MQTT client (esp_mqtt_client), event-driven, own
// task -- not PubSubClient/WiFiClient. Trimmed down from
// esp32scoringdeviceMqtt's AtlasAsyncMqttClient (same proven approach on
// this exact toolchain), dropping everything this bridge doesn't need
// (TLS/mTLS, NVS-stored certs). Switched to after PubSubClient's
// WiFiClient-based subscribe/receive path was confirmed not delivering
// any incoming messages at all on this arduino-esp32 2.0.14 / ESP-IDF
// 4.4.6 combo -- subscribe() reported success but the callback never
// fired, even for a trivial unrelated test topic.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include <string>

#include "mqtt_client.h"

using mqtt_connect_cb_t = std::function<void(bool sessionPresent)>;
using mqtt_message_cb_t =
    std::function<void(const char* topic, const char* payload, size_t len)>;

class Esp32MqttClient {
public:
    void setServer(const std::string& host, uint16_t port);
    void setClientId(const char* clientId);
    void setWill(const char* topic, const char* message, int qos, bool retain);

    void onConnect(mqtt_connect_cb_t cb);
    void onMessage(mqtt_message_cb_t cb);

    /// Starts the client -- connects, and keeps reconnecting on its own
    /// (esp_mqtt_client's built-in behavior) for as long as the process
    /// runs. Call once; the onConnect callback fires on every (re)connect.
    void begin();

    /// Call every iteration from the main loop() task. Drains events queued
    /// by the MQTT task and invokes onConnect/onMessage callbacks from here
    /// -- NEVER from begin()/the event handler directly. esp_mqtt_client's
    /// own task has a small stack (confirmed on real hardware: "stack
    /// overflow in task mqtt_task", crash-reboot looping) that our
    /// callbacks' JSON building/parsing and multi-message publishAll()
    /// blow straight through. This task (Arduino's main loop) has ample
    /// stack instead.
    void loop();

    void publish(const char* topic, const char* payload, int qos, bool retain);
    void subscribe(const char* topic, int qos);

    bool connected() const { return m_connected; }

private:
    static constexpr size_t kMaxTopicLen   = 64;
    static constexpr size_t kMaxPayloadLen = 640;  // covers OPP2::JSON_SIZE_MAX (512) + margin
    static constexpr UBaseType_t kQueueDepth = 4;

    enum class QueuedEventType { CONNECTED, MESSAGE };

    struct QueuedEvent {
        QueuedEventType type;
        bool            sessionPresent;                // CONNECTED
        char            topic[kMaxTopicLen];            // MESSAGE
        char            payload[kMaxPayloadLen];        // MESSAGE
        size_t          payloadLen;                     // MESSAGE
    };

    static void eventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId,
                             void* eventData);
    void handleEvent(esp_mqtt_event_handle_t event);
    void enqueue(const QueuedEvent& evt);

    esp_mqtt_client_handle_t m_client    = nullptr;
    bool                     m_started   = false;
    bool                     m_connected = false;
    QueueHandle_t            m_eventQueue = nullptr;

    std::string m_host;
    uint16_t    m_port = 1883;
    std::string m_clientId;

    std::string m_lwtTopic;
    std::string m_lwtMessage;
    int         m_lwtQos    = 0;
    bool        m_lwtRetain = false;

    mqtt_connect_cb_t m_connectCb;
    mqtt_message_cb_t m_messageCb;

    // MQTT_EVENT_DATA arrives fragmented for payloads bigger than esp-mqtt's
    // internal buffer -- reassembled here (still on the MQTT task, but this
    // part is cheap: std::string::append, no JSON work) the same way
    // AtlasAsyncMqttClient does it, before being copied into a QueuedEvent.
    std::string m_incomingTopic;
    std::string m_incomingPayload;
};
