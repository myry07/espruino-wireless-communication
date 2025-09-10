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

static const char *TAG = "cam";
static httpd_handle_t s_server = NULL;

// ===== 选择你的板卡映射 =====
// #define USE_S3_EYE
#define USE_GENERIC_EXAMPLE

#ifdef USE_S3_EYE
// 参考常见 ESP32-S3-EYE（实际以你的版本原理图为准）
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
// 通用示例（**占位**）：请按你的摄像头排线定义修改！
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
        .xclk_freq_hz   = 20000000,             // 20MHz 常用；若不亮可试 10MHz/16MHz
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        // **关键：巡线建议灰度格式，避免 JPEG 解码开销**
        .pixel_format   = PIXFORMAT_GRAYSCALE,  // 或 PIXFORMAT_YUV422 / RGB565 / JPEG
        .frame_size     = FRAMESIZE_QQVGA,      // 160x120；也可 QVGA(320x240)
        .jpeg_quality   = 12,                   // 仅 JPEG 有效
        .fb_count       = 1,                    // 单缓冲更省内存
        .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t* s = esp_camera_sensor_get();
    // 基础参数：先自动增益/曝光，调好光照后可锁定
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_gain_ctrl(s, 1);       // 自动增益
    s->set_exposure_ctrl(s, 1);   // 自动曝光
    // s->set_gain_ctrl(s, 0);    // 想锁定时改为 0 并设置增益
    // s->set_exposure_ctrl(s, 0);// 锁曝光

    return ESP_OK;
}

static void analyze_frame_gray(const camera_fb_t* fb)
{
    // 简单示例：统计低于某阈值(黑)的像素比例，便于你确认黑白分布
    const uint8_t* p = fb->buf;
    const int total = fb->len; // 灰度模式下 1 字节/像素
    int black = 0;
    const uint8_t TH = 90;     // 简单阈值
    for (int i = 0; i < total; ++i) black += (p[i] < TH);
    float ratio = (float)black * 100.0f / (float)total;
    ESP_LOGI(TAG, "Gray stat: black<th(%d) = %.2f%%", TH, ratio);
}

void app_main(void)
{
    ESP_ERROR_CHECK(camera_init());
    // 先抓一帧再打印宽高和像素格式（不同 esp32-camera 版本不再提供 status.framesize_width/height）
    camera_fb_t* fb0 = esp_camera_fb_get();
    if (fb0) {
        ESP_LOGI(TAG, "Camera started. FrameSize=%dx%d, fmt=%d", fb0->width, fb0->height, (int)fb0->format);
        esp_camera_fb_return(fb0);
    } else {
        ESP_LOGW(TAG, "Camera started but first frame is NULL");
    }

    int64_t t0 = esp_timer_get_time();
    int frames = 0;

    while (1) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "fb NULL");
            continue;
        }

        // 做点最小处理（仅灰度模式下示例）
        if (fb->format == PIXFORMAT_GRAYSCALE) {
            analyze_frame_gray(fb);
        }

        esp_camera_fb_return(fb);
        frames++;

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            ESP_LOGI(TAG, "FPS=%d", frames);
            frames = 0;
            t0 = now;
        }

        // 可视情况适当让出 CPU
        // vTaskDelay(1);
    }
}