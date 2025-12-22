#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "storage_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t file_index;
    uint32_t offset;
} logstream_pointer_t;

typedef struct {
    uint16_t version;

    uint32_t num_unread_entries;

    logstream_pointer_t head;
    logstream_pointer_t tail;

    uint32_t crc8;
} logstream_meta_t;

typedef struct {
    bool initialized;
    storage_worker_t storage_handle;
} logger_t;

// Synchronous operation context (used with storage_worker callbacks)
typedef struct {
    SemaphoreHandle_t sem;    // shared semaphore to signal completion
    volatile bool completed;  // set true in callback
    esp_err_t status;         // result status
    size_t bytes_processed;   // bytes processed for write/append/read
    size_t read_len;          // read length for read ops
} op_sync_t;

typedef struct {
    char name[16];
    char dirpath[STORAGE_MAX_PATH];
    logger_t* logger;
    logstream_meta_t meta;
    // Synchronization primitives
    SemaphoreHandle_t op_sem;      // created in logstream_open, deleted in close
    SemaphoreHandle_t meta_mutex;  // protects concurrent access to metadata
    // Per-operation contexts (separated by op type)
    op_sync_t sync_read;
    op_sync_t sync_write;
    op_sync_t sync_append;
    op_sync_t sync_delete;
    op_sync_t sync_mkdir;
} logstream_t;

esp_err_t logger_init(mount_point_t* mount_point, logger_t* out);
esp_err_t logger_deinit(logger_t* logger);

esp_err_t logstream_open(logger_t* logger, const char* stream_name, logstream_t* out_stream);
esp_err_t logstream_close(logstream_t* stream);
esp_err_t logstream_put(logstream_t* stream, const uint8_t* payload, size_t len);
esp_err_t logstream_get_unread(logstream_t* stream, uint8_t* out, size_t out_size, size_t* bytes_read);
esp_err_t logstream_get_status(logstream_t* stream, logstream_meta_t* out_meta);

#ifdef __cplusplus
}
#endif