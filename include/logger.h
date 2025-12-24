#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "storage_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

// Synchronous operation context (used with storage_worker callbacks)
typedef struct {
    SemaphoreHandle_t sem;    // shared semaphore to signal completion
    volatile bool completed;  // set true in callback
    esp_err_t status;         // result status
    size_t bytes_processed;   // bytes processed for write/append/read
    size_t read_len;          // read length for read ops
} op_sync_t;

typedef struct {
    bool initialized;
    storage_worker_t storage_handle;
} logger_t;

esp_err_t logger_init(mount_point_t* mount_point, logger_t* out);
esp_err_t logger_deinit(logger_t* logger);

#ifdef __cplusplus
}
#endif