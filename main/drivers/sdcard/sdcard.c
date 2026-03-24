#include "sdcard.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card = NULL;

bool sdcard_mount(gpio_num_t clk, gpio_num_t cmd,
                  gpio_num_t d0,  gpio_num_t d1,
                  gpio_num_t d2,  gpio_num_t d3)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk  = clk;
    slot_cfg.cmd  = cmd;
    slot_cfg.d0   = d0;
    slot_cfg.d1   = d1;
    slot_cfg.d2   = d2;
    slot_cfg.d3   = d3;
    slot_cfg.width = 4;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host,
                                             &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    return true;
}

void sdcard_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

int sdcard_next_index(void)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) return 1;

    int max_idx = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int idx = 0;
        // 匹配 AM_XXXX.wav
        if (sscanf(entry->d_name, "AM_%04d.wav", &idx) == 1) {
            if (idx > max_idx) max_idx = idx;
        }
    }
    closedir(dir);
    return max_idx + 1;
}
