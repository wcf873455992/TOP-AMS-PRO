#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dns_server.h"

#define DNS_PORT 53
#define DNS_MAX_LEN 512

static const char *TAG = "dns_hijack";
static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;
static char s_ap_ip[16] = "192.168.1.1";

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    while (1) {
        int len = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;

        buf[2] |= 0x80;   // QR=1
        buf[3] = 0x80;    // RA=1
        buf[6] = 0x00;    // ANCOUNT 高字节
        buf[7] = 0x01;    // ANCOUNT 低字节

        int pos = len;
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;   // NAME 指针
        buf[pos++] = 0x00; buf[pos++] = 0x01;   // TYPE A
        buf[pos++] = 0x00; buf[pos++] = 0x01;   // CLASS IN
        buf[pos++] = 0x00; buf[pos++] = 0x00;   // TTL
        buf[pos++] = 0x00; buf[pos++] = 0x3C;
        buf[pos++] = 0x00; buf[pos++] = 0x04;   // RDLENGTH

        struct in_addr addr;
        inet_aton(s_ap_ip, &addr);
        memcpy(&buf[pos], &addr.s_addr, 4);
        pos += 4;

        sendto(s_dns_sock, buf, pos, 0,
               (struct sockaddr *)&from, fromlen);
    }
}

void dns_server_start(const char *ap_ip)
{
    if (ap_ip) {
        strncpy(s_ap_ip, ap_ip, sizeof(s_ap_ip) - 1);
        s_ap_ip[sizeof(s_ap_ip) - 1] = '\0';
    }

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket 创建失败");
        return;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind 失败");
        close(s_dns_sock);
        s_dns_sock = -1;
        return;
    }

    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "DNS 劫持服务器已启动，目标 IP: %s", s_ap_ip);
}

void dns_server_stop(void)
{
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
}
