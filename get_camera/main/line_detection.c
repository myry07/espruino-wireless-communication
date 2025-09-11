// esp_cam_mjpeg_stream.c
// 在网页中实时显示摄像头画面（MJPEG 推流 + 简易网页）
// 适用于 ESP-IDF（v5.x 测试通过），基于 esp_http_server 与 esp32-camera。
// 使用方法：填写 Wi‑Fi SSID/PASS，烧录后在串口日志查看 IP，浏览器访问 http://<IP>/


#include <stdio.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"


/*

	•	http://192.168.1.103/（页面看实时画面）
	•	http://192.168.1.103/stream（裸 MJPEG）
	•	http://192.168.1.103/jpg（单帧抓拍）
    
    
*/

static const char *TAG = "cam";
static httpd_handle_t s_server = NULL;

// ===== Wi‑Fi 配置（STA）=====
#define WIFI_SSID "TP-LINK_C6BC"
#define WIFI_PASS "qwerasdf243"

// ===== 选择你的板卡映射 =====
// #define USE_S3_EYE
#define USE_GENERIC_EXAMPLE

#ifdef USE_S3_EYE
// 参考 ESP32‑S3‑EYE（按你实际硬件微调）
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 10
#define CAM_PIN_SIOD 40
#define CAM_PIN_SIOC 39
#define CAM_PIN_D7 48
#define CAM_PIN_D6 11
#define CAM_PIN_D5 12
#define CAM_PIN_D4 14
#define CAM_PIN_D3 16
#define CAM_PIN_D2 18
#define CAM_PIN_D1 17
#define CAM_PIN_D0 15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 13
#endif

#ifdef USE_GENERIC_EXAMPLE
// 通用占位：请按你的摄像头排线定义修改！
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D9 16
#define CAM_PIN_D8 17
#define CAM_PIN_D7 18
#define CAM_PIN_D6 12
#define CAM_PIN_D5 10
#define CAM_PIN_D4 8
#define CAM_PIN_D3 9
#define CAM_PIN_D2 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13
#endif

// ====== 简易首页 HTML（<img src="/stream"> 展示）======
static const char index_html[] =
    "<!doctype html><html><head>\n"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>ESP32 Camera</title>\n"
    "<style>body{margin:0;background:#111;color:#eee;font-family:system-ui}header{padding:10px 16px;position:sticky;top:0;background:#111;border-bottom:1px solid #222}main{display:flex;justify-content:center;padding:12px}img{max-width:100%;height:auto;background:#000}</style>\n"
    "</head><body>\n"
    "<header><h3>ESP32 Camera Stream</h3></header>\n"
    "<main><img id=\"v\" src=\"/stream\" alt=\"stream\"></main>\n"
    "</body></html>";

static esp_err_t wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA init... connecting to %s", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
    return ESP_OK;
}

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D9,
        .pin_d6 = CAM_PIN_D8,
        .pin_d5 = CAM_PIN_D7,
        .pin_d4 = CAM_PIN_D6,
        .pin_d3 = CAM_PIN_D5,
        .pin_d2 = CAM_PIN_D4,
        .pin_d1 = CAM_PIN_D3,
        .pin_d0 = CAM_PIN_D2,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000, // 常用 20MHz，可按需改 10/16MHz
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_YUV422,
        .frame_size = FRAMESIZE_QQVGA, // 320x240；可换 CIF/VGA 视性能
        .jpeg_quality = 12,            // 10~20 越小质量越高
        .fb_count = 2,                 // 双缓冲更流畅（需 PSRAM 更佳）
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_gain_ctrl(s, 1);     // 自动增益
        s->set_exposure_ctrl(s, 1); // 自动曝光

    }
    return ESP_OK;
}

// ===== HTTP Handlers =====
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// multipart/x-mixed-replace MJPEG 流
static esp_err_t stream_get_handler(httpd_req_t *req)
{
    static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
    static const char *_BOUNDARY = "\r\n--frame\r\n";
    static const char *_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGW(TAG, "fb NULL");
            continue; // 或者 break; 根据需要
        }

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;

        if (fb->format == PIXFORMAT_JPEG)
        {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        }
        else
        {
            // 兜底：非 JPEG 时转换
            if (!frame2jpg(fb, 80, &jpg_buf, &jpg_len))
            {
                ESP_LOGE(TAG, "frame2jpg failed");
                esp_camera_fb_return(fb);
                break;
            }
        }

        // 先发 boundary
        if (httpd_resp_send_chunk(req, _BOUNDARY, strlen(_BOUNDARY)) != ESP_OK)
        {
            if (fb->format != PIXFORMAT_JPEG && jpg_buf)
                free(jpg_buf);
            esp_camera_fb_return(fb);
            break;
        }

        // 发头部（每帧）
        char part_buf[64];
        int hlen = snprintf(part_buf, sizeof(part_buf), _PART, (unsigned)jpg_len);
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK)
        {
            if (fb->format != PIXFORMAT_JPEG && jpg_buf)
                free(jpg_buf);
            esp_camera_fb_return(fb);
            break;
        }

        // 发图像数据
        if (httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len) != ESP_OK)
        {
            if (fb->format != PIXFORMAT_JPEG && jpg_buf)
                free(jpg_buf);
            esp_camera_fb_return(fb);
            break;
        }

        if (fb->format != PIXFORMAT_JPEG && jpg_buf)
        {
            free(jpg_buf);
        }
        esp_camera_fb_return(fb);

        // 可按需限速：
        // vTaskDelay(pdMS_TO_TICKS(5));
    }

    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    if (httpd_start(&s_server, &config) == ESP_OK)
    {
        httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};
        httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(s_server, &index_uri);
        httpd_register_uri_handler(s_server, &stream_uri);
        ESP_LOGI(TAG, "HTTP server started on :%d", config.server_port);
    }
    else
    {
        ESP_LOGE(TAG, "HTTP server start failed");
    }
    return s_server;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wifi_init_sta());

    // 等待获取 IP（简单轮询，也可用事件回调）
    for (int i = 0; i < 50; ++i)
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_ERROR_CHECK(camera_init());

    // 打印首帧信息
    camera_fb_t *fb0 = esp_camera_fb_get();
    if (fb0)
    {
        ESP_LOGI(TAG, "Camera ok. %dx%d fmt=%d size=%u", fb0->width, fb0->height, fb0->format, (unsigned)fb0->len);
        esp_camera_fb_return(fb0);
    }

    start_webserver();

    // 主任务空转（HTTP 服务器与摄像头在各自回调内工作）
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
