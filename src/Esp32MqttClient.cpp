#include "Esp32MqttClient.h"

#include <esp_log.h>
#include <cstring>

namespace {
const char* TAG = "mqtt";
}

void Esp32MqttClient::setServer(const std::string& host, uint16_t port) {
    m_host = host;
    m_port = port;
}

void Esp32MqttClient::setClientId(const char* clientId) { m_clientId = clientId; }

void Esp32MqttClient::setWill(const char* topic, const char* message, int qos,
                              bool retain) {
    m_lwtTopic   = topic;
    m_lwtMessage = message;
    m_lwtQos     = qos;
    m_lwtRetain  = retain;
}

void Esp32MqttClient::onConnect(mqtt_connect_cb_t cb) { m_connectCb = std::move(cb); }
void Esp32MqttClient::onMessage(mqtt_message_cb_t cb) { m_messageCb = std::move(cb); }

void Esp32MqttClient::publish(const char* topic, const char* payload, int qos,
                              bool retain) {
    if (!m_client || !m_connected) return;
    esp_mqtt_client_publish(m_client, topic, payload, 0, qos, retain);
}

void Esp32MqttClient::subscribe(const char* topic, int qos) {
    if (!m_client || !m_connected) return;
    esp_mqtt_client_subscribe(m_client, topic, qos);
}

void Esp32MqttClient::begin() {
    if (m_started) return;

    m_eventQueue = xQueueCreate(kQueueDepth, sizeof(QueuedEvent));

    esp_mqtt_client_config_t cfg = {};
    cfg.host      = m_host.c_str();
    cfg.port      = m_port;
    cfg.transport = MQTT_TRANSPORT_OVER_TCP;
    cfg.client_id = m_clientId.empty() ? nullptr : m_clientId.c_str();

    if (!m_lwtTopic.empty()) {
        cfg.lwt_topic  = m_lwtTopic.c_str();
        cfg.lwt_msg    = m_lwtMessage.c_str();
        cfg.lwt_qos    = m_lwtQos;
        cfg.lwt_retain = m_lwtRetain;
    }

    m_client = esp_mqtt_client_init(&cfg);
    if (!m_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return;
    }

    esp_mqtt_client_register_event(m_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                   &Esp32MqttClient::eventHandler, this);

    const esp_err_t err = esp_mqtt_client_start(m_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %d", err);
        return;
    }

    m_started = true;
}

void Esp32MqttClient::enqueue(const QueuedEvent& evt) {
    if (!m_eventQueue) return;
    // Never block the MQTT task waiting for queue space -- drop instead.
    if (xQueueSend(m_eventQueue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping event (type=%d)",
                static_cast<int>(evt.type));
    }
}

void Esp32MqttClient::loop() {
    if (!m_eventQueue) return;

    QueuedEvent evt;
    while (xQueueReceive(m_eventQueue, &evt, 0) == pdTRUE) {
        if (evt.type == QueuedEventType::CONNECTED) {
            if (m_connectCb) m_connectCb(evt.sessionPresent);
        } else {
            if (m_messageCb) m_messageCb(evt.topic, evt.payload, evt.payloadLen);
        }
    }
}

void Esp32MqttClient::eventHandler(void* handlerArgs, esp_event_base_t /*base*/,
                                   int32_t /*eventId*/, void* eventData) {
    static_cast<Esp32MqttClient*>(handlerArgs)->handleEvent(
        static_cast<esp_mqtt_event_handle_t>(eventData));
}

// Runs on esp_mqtt_client's own task, which has a small stack -- confirmed
// on real hardware ("stack overflow in task mqtt_task", crash-reboot
// looping) when this used to call onConnect/onMessage (and therefore
// publishAll()'s seven synchronous JSON builds, or JSON deserialization)
// directly from here. Only cheap, fixed-size-copy work happens in this
// function now; everything else is queued for loop() to run on the main
// task's much larger stack.
void Esp32MqttClient::handleEvent(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "connected");
            m_connected = true;
            QueuedEvent evt{};
            evt.type           = QueuedEventType::CONNECTED;
            evt.sessionPresent = event->session_present;
            enqueue(evt);
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "disconnected");
            m_connected = false;
            break;

        case MQTT_EVENT_DATA:
            // Payloads bigger than esp-mqtt's internal buffer arrive split
            // across several MQTT_EVENT_DATA events; current_data_offset==0
            // marks the first fragment (the only one carrying the topic).
            if (event->current_data_offset == 0) {
                m_incomingTopic.assign(event->topic, event->topic_len);
                m_incomingPayload.clear();
                m_incomingPayload.reserve(event->total_data_len);
            }
            m_incomingPayload.append(event->data, event->data_len);

            if (event->current_data_offset + event->data_len >= event->total_data_len) {
                if (m_incomingTopic.size() < kMaxTopicLen &&
                    m_incomingPayload.size() < kMaxPayloadLen) {
                    QueuedEvent evt{};
                    evt.type = QueuedEventType::MESSAGE;
                    memcpy(evt.topic, m_incomingTopic.c_str(), m_incomingTopic.size() + 1);
                    memcpy(evt.payload, m_incomingPayload.c_str(),
                          m_incomingPayload.size() + 1);
                    evt.payloadLen = m_incomingPayload.size();
                    enqueue(evt);
                } else {
                    ESP_LOGW(TAG, "message too large to queue (topic=%u payload=%u)",
                            static_cast<unsigned>(m_incomingTopic.size()),
                            static_cast<unsigned>(m_incomingPayload.size()));
                }
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "subscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}
