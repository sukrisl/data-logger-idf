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

// TODO: API of log streams
esp_err_t logger_register_stream(const char* stream_name, logger_t* logger);
esp_err_t logger_unregister_stream(const char* stream_name, logger_t* logger);

esp_err_t logger_put_entry(logger_t* logger, const char* stream_name, const uint8_t* payload, size_t len);
esp_err_t logger_get_entries(logger_t* logger, const char* stream_name, uint8_t* out, size_t out_size,
                             size_t* bytes_read);
esp_err_t logger_mark_entries_read(logger_t* logger, const char* stream_name, void*);

#define APP_LOG(stream, format, ...)                                                                                \
    do {                                                                                                            \
        esp_err_t err = logger_put_entry(stream, (const uint8_t*)format, snprintf(NULL, 0, format, ##__VA_ARGS__)); \
        if (err != ESP_OK) { /* handle error */                                                                     \
        }                                                                                                           \
    } while (0)

#define APP_LOG_GET(stream, out, out_size, bytes_read)                         \
    do {                                                                       \
        esp_err_t err = logger_get_entries(stream, out, out_size, bytes_read); \
        if (err != ESP_OK) { /* handle error */                                \
        } else {                                                               \
            err = logger_mark_entries_read(stream, out, out_size, bytes_read); \
            if (err != ESP_OK) { /* handle error */                            \
            }                                                                  \
        }                                                                      \
    } while (0)

#ifdef __cplusplus
}
#endif