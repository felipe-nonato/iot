#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/event_groups.h"

// Configurações de Rede e Broker
#define WIFI_SSID      "FAMILIA_FTN"
#define WIFI_PASS      "HouseOfCats"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

#define BROKER_URI     "mqtt://192.168.3.4"

// Configurações de Hardware
#define LED_GPIO       13 // Pino do LED físico (com resistor de 220R)

static const char *TAG = "NODE_B_SUBSCRIBER";
esp_mqtt_client_handle_t mqtt_client = NULL;

// Tarefa do Desafio Extra: Status Keep Alive a cada 30 segundos
void keep_alive_task(void *pvParameters) {
    while (1) {
        if (mqtt_client != NULL) {
            const char *msg = "Node B (Atuador): ONLINE";
            esp_mqtt_client_publish(mqtt_client, "ifpb/projeto/status", msg, 0, 1, 0);
            ESP_LOGI(TAG, "Mensagem de status publicada.");
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao Broker MQTT!");
            // Inscrever-se no tópico imediatamente após conexão bem-sucedida
            esp_mqtt_client_subscribe(event->client, "ifpb/projeto/led", 0);
            ESP_LOGI(TAG, "Inscrição realizada no tópico: ifpb/projeto/led");
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Mensagem recebida no tópico: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Dados: %.*s", event->data_len, event->data);
            
            // Tratamento seguro da string recebida
            char payload[10];
            int len = event->data_len < 9 ? event->data_len : 9;
            memcpy(payload, event->data, len);
            payload[len] = '\0';
            
            // Compara a string recebida para ligar ou desligar o GPIO do LED
            if (strcmp(payload, "ON") == 0) {
                gpio_set_level(LED_GPIO, 1);
                ESP_LOGI(TAG, "Comando executado: LED LIGADO");
            } else if (strcmp(payload, "OFF") == 0) {
                gpio_set_level(LED_GPIO, 0);
                ESP_LOGI(TAG, "Comando executado: LED DESLIGADO");
            }
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
    ESP_LOGI(TAG, "Iniciando Node B (Atuador)");
    
    // Configurar o pino do LED como saída
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
    
    // 1. Inicialização NVS e conexão à mesma rede Wi-Fi configurada
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    
	esp_netif_init();
    esp_event_loop_create_default();
	
	// Inicia e conecta no Wi-Fi ANTES de chamar o MQTT
	wifi_init_sta(); 

    
    // 2. Conectar-se ao Broker MQTT
    mqtt_app_start();
    
    // 3. Iniciar tarefa do desafio extra (Keep Alive)
    xTaskCreate(keep_alive_task, "keep_alive_task", 2048, NULL, 5, NULL);
}