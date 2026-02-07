#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "driver/gpio.h"
// #include "led_strip.h"

#define PORT 3333
#define EXAMPLE_ESP_WIFI_SSID      "Eugenexn"
#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_ESP_MAXIMUM_RETRY  10

// GPIO Definitions
#define MOTOR_ENA_GPIO 8
#define MOTOR_IN1_GPIO 4
#define MOTOR_IN2_GPIO 5
#define LED1_GPIO 12
#define LED2_GPIO 13
#define BOOT_BUTTON_GPIO 9

static const char *TAG = "tcp_server";

static void configure_motor_driver(void)
{
    // Configure ENA
    gpio_reset_pin(MOTOR_ENA_GPIO);
    gpio_set_direction(MOTOR_ENA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MOTOR_ENA_GPIO, 0); // Disable by default

    // Configure IN1
    gpio_reset_pin(MOTOR_IN1_GPIO);
    gpio_set_direction(MOTOR_IN1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MOTOR_IN1_GPIO, 0);

    // Configure IN2
    gpio_reset_pin(MOTOR_IN2_GPIO);
    gpio_set_direction(MOTOR_IN2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MOTOR_IN2_GPIO, 0);

    // Configure LED1
    gpio_reset_pin(LED1_GPIO);
    gpio_set_direction(LED1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED1_GPIO, 0);

    // Configure LED2
    gpio_reset_pin(LED2_GPIO);
    gpio_set_direction(LED2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED2_GPIO, 0);
}

// Global socket handle for sending from other tasks
static int g_client_sock = -1;

static void button_task(void *pvParameters)
{
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    // Flash LEDs to indicate system ready
    gpio_set_level(LED1_GPIO, 1);
    gpio_set_level(LED2_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(LED1_GPIO, 0);
    gpio_set_level(LED2_GPIO, 0);

    int last_state = 1;

    while (1) {
        int state = gpio_get_level(BOOT_BUTTON_GPIO);
        
        // 只要按下就打印，不管是长按还是短按，先确保能读到
        if (state == 0) {
            ESP_LOGW("BTN", "GPIO 9 is LOW! (Button Pressed)");
            
            // 下降沿发送
            if (last_state == 1) {
                if (g_client_sock != -1) {
                     const char* msg = "Button Pressed!\n";
                     send(g_client_sock, msg, strlen(msg), 0);
                     ESP_LOGI("BTN", "Sent message to PC");
                } else {
                     ESP_LOGI("BTN", "Button pressed but no client connected");
                }
                vTaskDelay(pdMS_TO_TICKS(200)); // Debounce
            }
        }
        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = 5;
    int keepInterval = 5;
    int keepCount = 3;
    struct sockaddr_storage dest_addr;

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(PORT);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {
        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        
        // Use select/poll or just set socket to non-blocking to handle button press?
        // Simpler way: Accept is blocking, but we can check button inside a separate loop or making socket non-blocking.
        // For simplicity in this demo, we use a timeout on accept or just accept blocking and handle button in send loop?
        // Actually, if we block on accept, we can't detect button unless connected.
        // Let's assume we test button ONLY when connected for now to keep code simple.
        
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        // Set socket to non-blocking to handle button + receive simultaneously
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }

        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);
        
        // Update global socket for button task
        g_client_sock = sock;

        char rx_buffer[128];

        while (1) {
            // 1. Try to receive data (Non-blocking)
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len > 0) {
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
                
                char cmd = rx_buffer[0];
                const char* reply_msg = NULL;
                static int next_led = 0; // 0: LED1, 1: LED2

                if (cmd == 'L' || cmd == 'l') {
                     int pin = (next_led == 0) ? LED1_GPIO : LED2_GPIO;
                     ESP_LOGI(TAG, "Turning ON LED GPIO %d", pin);
                     
                     gpio_set_level(pin, 1);
                     vTaskDelay(pdMS_TO_TICKS(1000));
                     gpio_set_level(pin, 0);
                     
                     reply_msg = (next_led == 0) ? "LED1 Blinked\n" : "LED2 Blinked\n";
                     next_led = !next_led;
                }
                else if (cmd == 'F' || cmd == 'f') {
                    // Forward: ENA=1, IN1=0, IN2=1 (User Requested Config)
                    gpio_set_level(MOTOR_IN1_GPIO, 0);
                    gpio_set_level(MOTOR_IN2_GPIO, 1);
                    gpio_set_level(MOTOR_ENA_GPIO, 1);

                    // Blink LEDs briefly
                    gpio_set_level(LED1_GPIO, 1);
                    gpio_set_level(LED2_GPIO, 1);
                    vTaskDelay(pdMS_TO_TICKS(100)); // Visible Blink
                    gpio_set_level(LED1_GPIO, 0);
                    gpio_set_level(LED2_GPIO, 0);

                    reply_msg = "Motor Forward (F)\n";
                    ESP_LOGI(TAG, "Motor FORWARD: IN1=0, IN2=1, ENA=1");
                } 
                else if (cmd == 'R' || cmd == 'r') { 
                    // Reverse: ENA=1, IN1=1, IN2=0 (Opposite of Forward)
                    gpio_set_level(MOTOR_IN1_GPIO, 1);
                    gpio_set_level(MOTOR_IN2_GPIO, 0);
                    gpio_set_level(MOTOR_ENA_GPIO, 1);
                    
                    // Blink LEDs briefly
                    gpio_set_level(LED1_GPIO, 1);
                    gpio_set_level(LED2_GPIO, 1);
                    vTaskDelay(pdMS_TO_TICKS(100)); // Visible Blink
                    gpio_set_level(LED1_GPIO, 0);
                    gpio_set_level(LED2_GPIO, 0);

                    reply_msg = "Motor Reverse (R)\n";
                    ESP_LOGI(TAG, "Motor REVERSE: IN1=1, IN2=0, ENA=1");
                }
                else if (cmd == 'S' || cmd == 's') { // Stop
                    // Stop: ENA=0
                    gpio_set_level(MOTOR_IN1_GPIO, 0);
                    gpio_set_level(MOTOR_IN2_GPIO, 0);
                    gpio_set_level(MOTOR_ENA_GPIO, 0);

                    // Blink LEDs briefly
                    gpio_set_level(LED1_GPIO, 1);
                    gpio_set_level(LED2_GPIO, 1);
                    vTaskDelay(pdMS_TO_TICKS(100)); // Visible Blink
                    gpio_set_level(LED1_GPIO, 0);
                    gpio_set_level(LED2_GPIO, 0);

                    reply_msg = "Motor Stopped\n";
                    ESP_LOGI(TAG, "Motor STOP");
                }
                
                if (reply_msg) {
                    send(sock, reply_msg, strlen(reply_msg), 0);
                }
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            } else {
                 if (errno != EAGAIN && errno != EWOULDBLOCK) {
                     ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
                     break;
                 }
            }
            
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        g_client_sock = -1; // Client disconnected

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket");
            shutdown(sock, 0);
            close(sock);
        }
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_ESP_MAXIMUM_RETRY  10

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "Disconnect reason: %d", disconnected->reason);

        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            ESP_LOGI(TAG, "connect to the AP fail");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        
        ESP_LOGI(TAG, "Starting TCP server task...");
        xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 5, NULL);
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize HW peripherals here to avoid double-init on wifi reconnect
    configure_motor_driver();
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
}
