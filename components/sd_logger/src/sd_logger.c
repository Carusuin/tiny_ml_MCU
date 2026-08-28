#include "sd_logger.h"

#include <stdio.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_LOGGER";

#define SD_MOUNT_POINT "/sdcard"
#define SD_CS_GPIO 5
#define SD_SCK_GPIO 18
#define SD_MOSI_GPIO 23
#define SD_MISO_GPIO 19

static FILE *log_file;
static bool sd_available;

bool sd_logger_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(err));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_GPIO;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unavailable: %s", esp_err_to_name(err));
        sd_available = false;
        return false;
    }

    log_file = fopen(SD_MOUNT_POINT "/ecu_log.csv", "a");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "Could not open " SD_MOUNT_POINT "/ecu_log.csv");
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        sd_available = false;
        return false;
    }

    struct stat file_stat;
    if (stat(SD_MOUNT_POINT "/ecu_log.csv", &file_stat) == 0 && file_stat.st_size == 0) {
        fprintf(log_file, "timestamp_ms,ecu_connected,rpm,speed_kph,coolant_temp_c,dtc_count\n");
        fflush(log_file);
    }

    sd_available = true;
    ESP_LOGI(TAG, "SD card mounted; logging to " SD_MOUNT_POINT "/ecu_log.csv");
    return true;
}

bool sd_logger_is_available(void)
{
    return sd_available;
}

bool sd_logger_log_ecu_status(uint32_t timestamp_ms,
                              bool ecu_connected,
                              uint16_t rpm,
                              uint16_t speed_kph,
                              uint8_t coolant_temp_c,
                              uint8_t dtc_count)
{
    if (!sd_available || log_file == NULL) {
        return false;
    }

    int written = fprintf(log_file, "%lu,%d,%u,%u,%u,%u\n",
                          (unsigned long)timestamp_ms,
                          ecu_connected ? 1 : 0,
                          rpm,
                          speed_kph,
                          coolant_temp_c,
                          dtc_count);
    if (written < 0) {
        ESP_LOGE(TAG, "SD write failed; disabling logger");
        fclose(log_file);
        log_file = NULL;
        sd_available = false;
        return false;
    }

    fflush(log_file);
    return true;
}
