#include <stdio.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"

#include "esp_http_server.h"
#include "img_converters.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "cam";

// ===== 选择你的板卡映射 =====
// #define USE_S3_EYE
#define USE_GENERIC_EXAMPLE

#ifdef USE_S3_EYE
// ...（此处省略，与原代码相同）
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

#define UART_PORT UART_NUM_1
#define UART_TXD_PIN 48 // 你给的引脚
#define UART_RXD_PIN 47
#define UART_BAUD 115200

typedef struct
{
    uint8_t th;
    int w, h;
    int tx, ty;
    bool ok_t;
    int mx, my;
    bool ok_m;
    int bx, by;
    bool ok_b;
    uint64_t ts_us;
} line_msg_t;

static QueueHandle_t line_q = NULL;

typedef struct
{
    float angle_deg; // 相对竖直的角度（右正）
    float offset_px; // 横向偏差（右正）
    uint64_t ts_us;  // 时间戳
} angle_msg_t;

static QueueHandle_t angle_tx_q = NULL;

static bool roi_line_centroid_gray(
    const uint8_t *buf, int w, int h,
    int y0, int y1,         // [y0, y1) 垂直范围
    uint8_t th,             // 阈值：像素 < th 视为“黑线前景”
    int min_pixels,         // 前景像素最小数量（避免噪声）
    int *out_x, int *out_y) // 结果（找不到时返回 false）
{
    // 边界保护
    if (y0 < 0)
        y0 = 0;
    if (y1 > h)
        y1 = h;
    if (y1 <= y0)
        return false;

    // 计算前景像素的加权质心
    // 这里使用所有前景像素的几何中心：cx = sum(x) / N, cy = sum(y) / N
    // 也可按像素强度权重，但此处为二值假设，简单稳妥。
    uint32_t sumx = 0, sumy = 0, cnt = 0;

    for (int y = y0; y < y1; ++y)
    {
        const uint8_t *row = buf + y * w;
        for (int x = 0; x < w; ++x)
        {
            // 灰度更暗代表黑线：小于阈值则计入
            if (row[x] < th)
            {
                sumx += (uint32_t)x;
                sumy += (uint32_t)y;
                cnt++;
            }
        }
    }

    if ((int)cnt < min_pixels)
        return false;
    *out_x = (int)(sumx / cnt);
    *out_y = (int)(sumy / cnt);
    return true;
}

// 可选：简单自适应阈值（按全帧均值减去偏置）
static uint8_t estimate_threshold_mean_offset(const uint8_t *buf, int len, int offset)
{
    uint32_t s = 0;
    for (int i = 0; i < len; ++i)
        s += buf[i];
    int mean = (int)(s / (uint32_t)len);
    int t = mean - offset; // 偏置视环境调节，推荐 15~40
    if (t < 0)
        t = 0;
    if (t > 255)
        t = 255;
    return (uint8_t)t;
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
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_GRAYSCALE, // 灰度：1字节/像素，处理最简单
        .frame_size = FRAMESIZE_QQVGA,
        .jpeg_quality = 12,
        .fb_count = 1,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_gain_ctrl(s, 1);     // 自动增益
    s->set_exposure_ctrl(s, 1); // 自动曝光
    return ESP_OK;
}

void find_line_task(void *arg)
{
    camera_fb_t *fb0 = esp_camera_fb_get();
    if (fb0)
    {
        ESP_LOGI(TAG, "Camera started. FrameSize=%dx%d, fmt=%d",
                 fb0->width, fb0->height, (int)fb0->format);
        esp_camera_fb_return(fb0);
    }
    else
    {
        ESP_LOGW(TAG, "Camera started but first frame is NULL");
    }

    int64_t t0 = esp_timer_get_time();
    int frames = 0;

    // 参数：阈值与最小前景像素数（按画面大小调）
    int adaptive_offset = 25; // 均值减去这个偏置得到阈值
    int min_pixels = 800;     // 每个 ROI 至少这么多像素才认为“找到线”

    while (1)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGW(TAG, "fb NULL");
            continue;
        }

        if (fb->format == PIXFORMAT_GRAYSCALE)
        {
            const uint8_t *buf = fb->buf;
            int w = fb->width;
            int h = fb->height;

            // 估计阈值（也可改成固定值，比如 90）
            uint8_t th = estimate_threshold_mean_offset(buf, w * h, adaptive_offset);

            // 切分成上/中/下三个 ROI
            int y_top0 = 0, y_top1 = h / 3;
            int y_mid0 = h / 3, y_mid1 = (2 * h) / 3;
            int y_bot0 = (2 * h) / 3, y_bot1 = h;

            int tx = -1, ty = -1, mx = -1, my = -1, bx = -1, by = -1;
            bool ok_t = roi_line_centroid_gray(buf, w, h, y_top0, y_top1, th, min_pixels, &tx, &ty);
            bool ok_m = roi_line_centroid_gray(buf, w, h, y_mid0, y_mid1, th, min_pixels, &mx, &my);
            bool ok_b = roi_line_centroid_gray(buf, w, h, y_bot0, y_bot1, th, min_pixels, &bx, &by);

            // 打印三个坐标
            ESP_LOGI(TAG, "TH=%u, TOP:(%d,%d)%s  MID:(%d,%d)%s  BOT:(%d,%d)%s",
                     th,
                     tx, ty, ok_t ? "" : " !",
                     mx, my, ok_m ? "" : " !",
                     bx, by, ok_b ? "" : " !");

            // 这里你可以继续：根据三个点计算横向误差/拟合直线/做 PID 控制
            // 例如：优先使用下方 ROI（更近处）:
            // int center = w/2;
            // int error = ok_b ? (bx - center)
            //                  : (ok_m ? (mx - center)
            //                          : (ok_t ? (tx - center) : 0));
            // 控制逻辑...

            if (line_q)
            {
                line_msg_t m = {
                    .th = th, .w = w, .h = h, .tx = tx, .ty = ty, .ok_t = ok_t, .mx = mx, .my = my, .ok_m = ok_m, .bx = bx, .by = by, .ok_b = ok_b, .ts_us = esp_timer_get_time()};
                xQueueOverwrite(line_q, &m); // 队列长度=1时，用最新覆盖旧数据
            }
        }

        esp_camera_fb_return(fb);
        frames++;

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000)
        {
            ESP_LOGI(TAG, "FPS=%d", frames);
            frames = 0;
            t0 = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void cal_angle_task(void *arg)
{
    line_msg_t m;

    // （可选）简单平滑
    float ang_ema = 0.0f, off_ema = 0.0f;
    const float ALPHA = 0.3f; // 0~1，越大越灵敏

    while (1)
    {
        if (xQueueReceive(line_q, &m, portMAX_DELAY) != pdTRUE)
            continue;

        // 1) 选择用于计算的两点：优先 “近处优先” BOT<->MID
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        bool have = false;

        if (m.ok_b && m.ok_m)
        {
            x1 = m.mx;
            y1 = m.my;
            x2 = m.bx;
            y2 = m.by;
            have = true;
        }
        else if (m.ok_m && m.ok_t)
        {
            x1 = m.tx;
            y1 = m.ty;
            x2 = m.mx;
            y2 = m.my;
            have = true;
        }
        else if (m.ok_b && m.ok_t)
        {
            x1 = m.tx;
            y1 = m.ty;
            x2 = m.bx;
            y2 = m.by;
            have = true;
        }

        float ang_deg_v = 0.0f; // 相对竖直的角度（右偏为正）
        float off_px = 0.0f;    // 横向偏差（以图像中心为0，右正左负）

        if (have)
        {
            float dx = (float)(x2 - x1);
            float dy = (float)(y2 - y1);
            if (fabsf(dy) < 1e-6f)
                dy = (dy >= 0 ? 1e-6f : -1e-6f); // 防止除零

            // 2) 角度：相对竖直（更适合巡线转向）
            ang_deg_v = atanf(dx / dy) * 180.0f / (float)M_PI;

            // 3) 横向偏差：优先用底部点（更接近车身）
            int xc = m.w / 2;
            if (m.ok_b)
                off_px = (float)(m.bx - xc);
            else if (m.ok_m)
                off_px = (float)(m.mx - xc);
            else if (m.ok_t)
                off_px = (float)(m.tx - xc);
            else
                off_px = 0.0f;

            // 4) （可选）指数平滑
            ang_ema = ALPHA * ang_deg_v + (1.0f - ALPHA) * ang_ema;
            off_ema = ALPHA * off_px + (1.0f - ALPHA) * off_ema;
        }

        // 原始数据（你原来的打印）
        ESP_LOGI("angle",
                 "recv ts=%lluus th=%u sz=%dx%d | "
                 "TOP(%s):(%d,%d)  MID(%s):(%d,%d)  BOT(%s):(%d,%d)",
                 (unsigned long long)m.ts_us, m.th, m.w, m.h,
                 m.ok_t ? "OK" : "NA", m.tx, m.ty,
                 m.ok_m ? "OK" : "NA", m.mx, m.my,
                 m.ok_b ? "OK" : "NA", m.bx, m.by);

        if (have)
        {
            ESP_LOGI("angle", "angle_v=%.2f deg  offset=%.1f px  |  EMA: angle=%.2f deg, offset=%.1f px",
                     ang_deg_v, off_px, ang_ema, off_ema);
        }
        else
        {
            ESP_LOGW("angle", "not enough points to compute angle");
        }

        if (have && angle_tx_q)
        {
            angle_msg_t a = {
                .angle_deg = ang_deg_v,
                .offset_px = off_px,
                .ts_us = m.ts_us};
            // 覆盖式最新：队列长度可设为1，这里用非阻塞发送
            xQueueOverwrite(angle_tx_q, &a);
        }
    }
}

static void uart1_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // 安装驱动（只用TX，不收也可以。这里给 1KB TX buffer）
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TXD_PIN, UART_RXD_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void uart1_tx_task(void *arg)
{
    angle_msg_t a;
    char line[96];
    while (1)
    {
        if (xQueueReceive(angle_tx_q, &a, portMAX_DELAY) != pdTRUE)
            continue;
        // 文本协议：ANG,OFF,TS（你也可以换成二进制或JSON）
        int n = snprintf(line, sizeof(line),
                         "ANG=%.2f, OFF=%.1f, TS=%llu\r\n",
                         a.angle_deg, a.offset_px,
                         (unsigned long long)a.ts_us);
        if (n > 0)
            uart_write_bytes(UART_PORT, line, n);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(camera_init());

    line_q = xQueueCreate(1, sizeof(line_msg_t));
    configASSERT(line_q != NULL);

    uart1_init();
    angle_tx_q = xQueueCreate(1, sizeof(angle_msg_t));
    configASSERT(angle_tx_q != NULL);

    xTaskCreatePinnedToCore(find_line_task, "find line", 2048 * 5, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(uart1_tx_task, "uart1_tx", 2048, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(cal_angle_task, "angle task", 2048 * 3, NULL, 2, NULL, 0);
}