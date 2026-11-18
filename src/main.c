#include <stdio.h>
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_video.h"
#include "ia.h"
#include "ui.h"

#define NUM_BUFS 2

static const char *TAG = "main";

/* Appele par la tache video a chaque image : on la propose seulement au modele */
static void camera_frame_cb(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes,
                            uint32_t camera_buf_ves, size_t camera_buf_len)
{
    (void)camera_buf_index;
    (void)camera_buf_len;
    ia_submit_frame(camera_buf, camera_buf_hes, camera_buf_ves);
}

void app_main(void)
{
    /* Ecran : LVGL epingle sur le coeur 0, le coeur 1 reste a l'inference */
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = 1,
#else
        .double_buffer = 0,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
        }
    };
    disp_cfg.lvgl_port_cfg.task_affinity = 0;
    bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    ui_init();

    /* Modele */
    if (ia_init() != 0)
    {
        ESP_LOGE(TAG, "Modele non charge : arret");
        return;
    }
    ia_start();

    /* Camera */
    bsp_camera_start(NULL);

    int fd = app_video_open(BSP_CAMERA_DEVICE, APP_VIDEO_FMT_DRIVER_DEFAULT);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "Ouverture de la camera impossible");
        return;
    }
    if (app_video_set_bufs(fd, NUM_BUFS, NULL) != ESP_OK)
    {
        ESP_LOGE(TAG, "Allocation des tampons video impossible");
        return;
    }
    app_video_register_frame_operation_cb(camera_frame_cb);
    if (app_video_stream_task_start(fd, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "Demarrage du flux video impossible");
        return;
    }

    ESP_LOGI(TAG, "TAB_HSE pret");
}