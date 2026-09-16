#include "probe.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define PROBE_PERIOD_US (30 * 1000000)   /* полный круг проверок раз в 30 с */
#define CONNECT_TIMEOUT_MS 400
#define HTTP_TIMEOUT_MS 1500

static const char *TAG = "probe";

/* Порты, которые обычно что-то говорят о состоянии сервера. */
static const struct { uint16_t port; const char *name; } PORTS[PROBE_PORTS] = {
    {   22, "SSH" },
    {   80, "WEB" },
    {  443, "HTTPS" },
    { 3389, "RDP" },
    {  445, "SMB" },
    { 3306, "DB" },
};

static probe_state_t s_st;
static volatile uint32_t s_host_be;      /* адрес хоста в сетевом порядке */
static volatile bool s_have_host;

void probe_set_host(const char *ip)
{
    if (!ip || !ip[0]) {
        s_have_host = false;
        return;
    }
    struct in_addr a;
    if (inet_aton(ip, &a) == 0) {
        return;
    }
    if (a.s_addr != s_host_be) {
        s_host_be = a.s_addr;
        s_have_host = true;
        s_st.checked = false;          /* для нового адреса данные ещё не собраны */
        s_st.ports[0] = 0;
        s_st.http_ok = false;
        ESP_LOGI(TAG, "цель проверок: %s", ip);
    }
}

/* TCP-подключение: возвращает время установки в мс или -1 (закрыт/таймаут). */
static int tcp_probe(uint32_t ip_be, uint16_t port, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip_be;

    fcntl(sock, F_SETFL, O_NONBLOCK);
    int64_t t0 = esp_timer_get_time();
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(sock + 1, NULL, &wset, NULL, &tv);
        if (rc <= 0) {
            close(sock);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(sock);
            return -1;
        }
    }
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    close(sock);
    return ms;
}

/* Если на хосте есть веб-сервер — забираем код ответа и заголовок Server. */
static void http_probe(uint32_t ip_be)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = ip_be;

    fcntl(sock, F_SETFL, O_NONBLOCK);
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = { .tv_sec = 0, .tv_usec = CONNECT_TIMEOUT_MS * 1000 };
        if (select(sock + 1, NULL, &wset, NULL, &tv) <= 0) {
            close(sock);
            return;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(sock);
            return;
        }
    }
    const char *req = "GET / HTTP/1.0\r\nHost: host\r\nUser-Agent: inkmetrics\r\n\r\n";
    int64_t t0 = esp_timer_get_time();
    if (send(sock, req, strlen(req), 0) < 0) {
        close(sock);
        return;
    }
    char buf[512];
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT_MS / 1000,
                          .tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000 };
    rc = select(sock + 1, &rset, NULL, NULL, &tv);
    if (rc <= 0) {
        close(sock);
        return;
    }
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    s_st.http_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_st.http_ok = true;
    s_st.http_code = 0;
    if (strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) {
            s_st.http_code = atoi(sp + 1);
        }
    }
    const char *srv = strstr(buf, "\r\nServer: ");
    if (srv) {
        srv += 10;
        const char *nl = strstr(srv, "\r\n");
        size_t len = nl ? (size_t)(nl - srv) : strlen(srv);
        if (len >= sizeof(s_st.http_server)) {
            len = sizeof(s_st.http_server) - 1;
        }
        memcpy(s_st.http_server, srv, len);
        s_st.http_server[len] = 0;
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_have_host) {
            uint32_t ip = s_host_be;
            for (int i = 0; i < PROBE_PORTS; i++) {
                int ms = tcp_probe(ip, PORTS[i].port, CONNECT_TIMEOUT_MS);
                s_st.svc[i].port = PORTS[i].port;
                s_st.svc[i].name = PORTS[i].name;
                s_st.svc[i].open = (ms >= 0);
                s_st.svc[i].rtt_ms = (ms > 0) ? (uint32_t)ms : 0;
            }
            s_st.http_ok = false;
            s_st.http_code = 0;
            s_st.http_ms = 0;
            s_st.http_server[0] = 0;
            if (s_st.svc[1].open) {            /* порт 80 открыт — спрашиваем код ответа */
                http_probe(ip);
            }
            s_st.checked_s = (uint32_t)(esp_timer_get_time() / 1000000);
            s_st.host_known = true;
            /* строка для экрана: ТОЛЬКО открытые порты через запятую ("22,80,445") */
            s_st.ports[0] = 0;
            for (int i = 0; i < PROBE_PORTS; i++) {
                if (s_st.svc[i].open) {
                    size_t len = strlen(s_st.ports);
                    if (len + 8 >= sizeof(s_st.ports)) {
                        break;
                    }
                    snprintf(s_st.ports + len, sizeof(s_st.ports) - len, "%s%u",
                             len ? "," : "", (unsigned)s_st.svc[i].port);
                }
            }
            s_st.checked = true;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_US / 1000));
    }
}

void probe_get(probe_state_t *out)
{
    if (out) {
        memcpy(out, &s_st, sizeof(*out));
    }
}

void probe_init(void)
{
    for (int i = 0; i < PROBE_PORTS; i++) {
        s_st.svc[i].port = PORTS[i].port;
        s_st.svc[i].name = PORTS[i].name;
    }
    xTaskCreate(probe_task, "probe", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "проверка сервисов хоста: SSH/WEB/HTTPS/RDP/SMB/DB, раз в %d с",
             (int)(PROBE_PERIOD_US / 1000000));
}
