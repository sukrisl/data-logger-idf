#include "logger.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "string.h"

static const char* TAG = "data_logger";

static void logger_storage_cb(const storage_result_t* res, void* user_ctx) {
    // user_ctx points to per-operation sync context
    if (user_ctx == NULL) {
        return;
    }
    op_sync_t* sync = (op_sync_t*)user_ctx;
    sync->status = res->status;
    sync->bytes_processed = res->bytes_processed;
    sync->read_len = res->read_len;
    sync->completed = true;
    if (sync->sem) {
        xSemaphoreGive(sync->sem);
    }
    if (res->status != ESP_OK) {
        ESP_LOGE(TAG, "Storage op %d failed: %s (path: %s)", (int)res->op, esp_err_to_name(res->status), res->path);
    }
}

esp_err_t logger_init(mount_point_t* mount_point, logger_t* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out->initialized) {
        ESP_LOGW(TAG, "Logger already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // * Initialize storage worker
    esp_err_t err = partition_mount(mount_point);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount logger partition (%s): %s", mount_point->partition_label, esp_err_to_name(err));
        return err;
    }

    err = storage_init(mount_point, &out->storage_handle, 8, 8192, 0, logger_storage_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize storage worker(%s/%s): %s", mount_point->partition_label,
                 mount_point->base_path, esp_err_to_name(err));
        return err;
    }

    out->initialized = true;
    return ESP_OK;
}

esp_err_t logger_deinit(logger_t* logger) {
    if (!logger || !logger->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_deinit(&logger->storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize storage worker: %s", esp_err_to_name(err));
        return err;
    }

    err = partition_unmount(&logger->storage_handle.mount);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount logger partition (%s): %s", logger->storage_handle.mount.partition_label,
                 esp_err_to_name(err));
        return err;
    }

    logger->initialized = false;
    return ESP_OK;
}