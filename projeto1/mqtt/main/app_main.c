#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h" 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"

// Configurações de Rede e Broker conforme requisitos 
#define BROKER_URI     "mqtt://192.168.3.4"
#define TOPIC_LED      "ifpb/projeto/led"
#define TOPIC_STATUS   "ifpb/projeto/status"

// CONEXÃO WIFI
#define WIFI_SSID      "FAMILIA_FTN"
#define WIFI_PASS      "HouseOfCats"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Configurações de Hardware
#define BUTTON_GPIO  4

static const char *TAG = "NODE_A_PUB";
esp_mqtt_client_handle_t mqtt_client = NULL;
bool led_state = false;

// Desafio Extra: Status a cada 30 segundos
void keep_alive_task(void *pvParameters) {
    while (1) {
        if (mqtt_client != NULL) {
            char msg[64];
            int64_t uptime = esp_timer_get_time() / 1000000;
            snprintf(msg, sizeof(msg), "Node A Online - Uptime: %llds", uptime);
            
            esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, msg, 0, 1, 0);
            ESP_LOGI(TAG, "Status enviado: %s", msg);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// Tratamento do botão via Polling eficiente FreeRTOS 
void button_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE, // Pull-up interno ativado
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    // Como é Pull-Up, o estado inicial/ocioso do pino é 1 (HIGH)
    int last_state = 1;

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO);
        
        // Detecta borda de descida (botão pressionado: 1 -> 0)
        if (last_state == 1 && current_state == 0) {
            
            // Print garantido no terminal sempre que apertado, independente do MQTT
            ESP_LOGI(TAG, "Botão pressionado no pino D4!");

            led_state = !led_state;
            const char *payload = led_state ? "ON" : "OFF";
            
            if (mqtt_client != NULL) {
                // Publicação no tópico específico 
                esp_mqtt_client_publish(mqtt_client, TOPIC_LED, payload, 0, 1, 0);
                ESP_LOGI(TAG, "Mensagem publicada no MQTT: %s", payload);
            }
            vTaskDelay(pdMS_TO_TICKS(250)); // Tempo de Debounce
        }
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao Broker Mosquitto em 10.1.133.82"); 
            break;
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI("WIFI", "Tentando reconectar ao roteador...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI", "Conectado com sucesso! IP Recebido: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI("WIFI", "Aguardando conexão com %s...", WIFI_SSID);
    // Trava a execução aqui até o Wi-Fi conectar, garantindo que o MQTT só inicie com internet
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void) {
    // Inicialização básica 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    esp_netif_init();
    esp_event_loop_create_default();
    
	wifi_init_sta();

    mqtt_app_start(); 
    
    xTaskCreate(button_task, "button_task", 3072, NULL, 10, NULL); 
    xTaskCreate(keep_alive_task, "keep_alive_task", 3072, NULL, 5, NULL);
}