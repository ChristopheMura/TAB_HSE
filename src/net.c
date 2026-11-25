// Wi-Fi (ESP32-C6 via esp_hosted/esp_wifi_remote) + client MQTT.
#include "net.h"
#include "net_config.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "net";

static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool s_wifi_ok = false;
static volatile bool s_mqtt_ok = false;
static net_status_cb_t s_status_cb = NULL;
static net_autorisation_cb_t s_autor_cb = NULL;

void net_set_status_cb(net_status_cb_t cb) { s_status_cb = cb; }
void net_set_autorisation_cb(net_autorisation_cb_t cb) { s_autor_cb = cb; }
bool net_mqtt_connected(void) { return s_mqtt_ok; }

static void notify_status(void)
{
    if (s_status_cb) s_status_cb(s_wifi_ok, s_mqtt_ok);
}

/* ---------- Reception : {"id":1,"autorisation":true} ---------- */
static void handle_autorisation(const char *data, int len)
{
    char buf[256];
    if (len <= 0 || len >= (int)sizeof(buf))
    {
        ESP_LOGW(TAG, "Message Autorisation ignore (taille %d)", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "Autorisation : JSON invalide : %s", buf);
        return;
    }

    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(root, "Autorisation");

    if (!cJSON_IsNumber(id) || !cJSON_IsBool(val))
    {
        ESP_LOGW(TAG, "Autorisation : champ id ou autorisation absent/invalide : %s", buf);
        cJSON_Delete(root);
        return;
    }

    if ((int)id->valuedouble != NET_DEVICE_ID)
    {
        ESP_LOGW(TAG, "Autorisation ignoree : id %d, attendu %d", (int)id->valuedouble, NET_DEVICE_ID);
        cJSON_Delete(root);
        return;
    }

    const bool ok = cJSON_IsTrue(val);
    ESP_LOGI(TAG, "Autorisation recue (id %d) : %s", (int)id->valuedouble, ok ? "true" : "false");
    if (s_autor_cb) s_autor_cb(ok);

    cJSON_Delete(root);
}

/* ---------- MQTT ---------- */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)id) 
    {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_ok = true;
        ESP_LOGI(TAG, "MQTT connecte a %s", NET_MQTT_URI);
        esp_mqtt_client_subscribe(s_mqtt, NET_TOPIC_AUTORISATION, 1);
        notify_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_ok = false;
        ESP_LOGW(TAG, "MQTT deconnecte");
        notify_status();
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Abonne a '%s'", NET_TOPIC_AUTORISATION);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Publication acquittee (msg_id=%d)", ev->msg_id);
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(NET_TOPIC_AUTORISATION) &&
            strncmp(ev->topic, NET_TOPIC_AUTORISATION, ev->topic_len) == 0) {
            handle_autorisation(ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            ESP_LOGE(TAG, "Connexion refusee par le broker, code %d (4 = identifiants, 5 = non autorise)",
                ev->error_handle->connect_return_code);
        else
            ESP_LOGE(TAG, "Erreur MQTT (type %d)", ev->error_handle ? ev->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_mqtt != NULL) return;
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = NET_MQTT_URI,
        .credentials.client_id = NET_MQTT_CLIENT_ID,
        .credentials.username = NET_MQTT_USER,
        .credentials.authentication.password = NET_MQTT_PASS,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);   // reconnexion automatique geree par le client
}

/* ---------- Wi-Fi ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) 
    {
        esp_wifi_connect();
    } 
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        ESP_LOGW(TAG, "Wi-Fi deconnecte, nouvelle tentative");
        notify_status();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } 
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_wifi_ok = true;
        ESP_LOGI(TAG, "Wi-Fi connecte, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        notify_status();
        mqtt_start();
    }
}

int net_start(void)
{
    // 1. Alimentation du module ESP32-C6 (expandeur d'E/S de la Tab5)
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "Alimentation du C6 impossible : %s", esp_err_to_name(err));
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    // 2. NVS (requis par la pile Wi-Fi)
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "NVS : %s", esp_err_to_name(err));
        return -1;
    }

    // 3. Pile reseau + Wi-Fi station (les appels esp_wifi_* passent par le C6)
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "esp_wifi_init : %s (le C6 repond-il ?)", esp_err_to_name(err));
        return -1;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, NET_WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, NET_WIFI_PASSWORD, sizeof(sta.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    err = esp_wifi_start();
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "esp_wifi_start : %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Connexion Wi-Fi a '%s'...", NET_WIFI_SSID);
    return 0;
}

/* ---------- Publications ---------- */
static int publish(const char *topic, const char *json)
{
    if (!s_mqtt_ok || s_mqtt == NULL) 
    {
        ESP_LOGW(TAG, "Publication sur '%s' impossible : MQTT non connecte", topic);
        return -1;
    }
    // Mise en file non bloquante, QoS 1 : utilisable depuis LVGL ou la tache d'inference
    int id = esp_mqtt_client_enqueue(s_mqtt, topic, json, 0, 1, 0, true);
    ESP_LOGI(TAG, "-> %s : %s (msg_id=%d)", topic, json, id);
    return id < 0 ? -1 : 0;
}

int net_publish_demande_critique(void)
{
    return publish(NET_TOPIC_DEMANDE, "{\"id\":1,\"Bouton\":true}");
}

int net_publish_detection(bool casque, bool gilet, bool chaussures)
{
    char json[128];
    snprintf(json, sizeof(json),
         "{\"id\":1,\"Detection\":{\"Casque\":%s,\"Gilet\":%s,\"Chaussures\":%s}}",
         casque ? "true" : "false",
         gilet ? "true" : "false",
         chaussures ? "true" : "false");
    return publish(NET_TOPIC_DETECTION, json);
}