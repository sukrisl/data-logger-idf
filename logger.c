#include "logger.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "string.h"

static const char* TAG = "data_logger";

// Configuration constants
#define MAX_FILES_PER_STREAM 16
#define MAX_FILE_SIZE STORAGE_MAX_WRITE_SIZE
#define METADATA_VERSION 1
#define ENTRY_HEADER_SIZE 4
#define LOGSTREAM_SYNC_TIMEOUT_MS 1000

// Log entry header structure (4 bytes)
typedef struct __attribute__((packed)) {
    uint16_t length;    // Payload length
    uint16_t checksum;  // Simple checksum
} log_entry_header_t;

// Helper functions
static uint8_t calculate_meta_crc(const logstream_meta_t* meta) {
    return esp_crc8_le(0, (uint8_t*)meta, sizeof(logstream_meta_t) - sizeof(uint32_t));
}

static uint16_t calculate_entry_checksum(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static inline uint32_t ring_total_capacity_bytes(void) {
    return (uint32_t)MAX_FILES_PER_STREAM * (uint32_t)MAX_FILE_SIZE;
}

static inline uint32_t ring_abs_bytes(const logstream_pointer_t* ptr) {
    return ((uint32_t)ptr->file_index * (uint32_t)MAX_FILE_SIZE) + (uint32_t)ptr->offset;
}

// Forward distance from 'from' to 'to' when moving forward in the ring.
static inline uint32_t ring_forward_distance_bytes(const logstream_pointer_t* from, const logstream_pointer_t* to) {
    uint32_t total = ring_total_capacity_bytes();
    uint32_t a = ring_abs_bytes(from);
    uint32_t b = ring_abs_bytes(to);
    return (b + total - a) % total;
}

static void get_file_path(const logstream_t* stream, uint16_t file_index, char* out_path, size_t max_len) {
    snprintf(out_path, max_len, "%s/%u.log", stream->dirpath, file_index);
}

static void get_meta_path(const logstream_t* stream, char* out_path, size_t max_len) {
    snprintf(out_path, max_len, "%s/meta", stream->dirpath);
}

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

// Synchronous wrappers around storage_worker operations
static inline esp_err_t wait_for_sync(op_sync_t* sync, uint32_t timeout_ms) {
    if (!sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(sync->sem, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return sync->status;
}

static esp_err_t storage_write_sync(storage_worker_t* worker, const char* path, const void* data, size_t len,
                                    op_sync_t* sync) {
    if (!worker || !path || !data || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    // clear any previous signal
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    esp_err_t err = storage_write(worker, path, data, len, (void*)sync);
    if (err != ESP_OK) return err;
    // wait for completion
    err = wait_for_sync(sync, LOGSTREAM_SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "storage_write_sync timeout on '%s'", path);
    }
    return err;
}

static esp_err_t storage_append_sync(storage_worker_t* worker, const char* path, const void* data, size_t len,
                                     op_sync_t* sync) {
    if (!worker || !path || !data || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    esp_err_t err = storage_append(worker, path, data, len, (void*)sync);
    if (err != ESP_OK) return err;
    err = wait_for_sync(sync, LOGSTREAM_SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "storage_append_sync timeout on '%s'", path);
    }
    return err;
}

static esp_err_t storage_read_sync(storage_worker_t* worker, const char* path, void* buf, size_t buf_size,
                                   op_sync_t* sync) {
    if (!worker || !path || !buf || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    esp_err_t err = storage_read(worker, path, buf, buf_size, (void*)sync);
    if (err != ESP_OK) return err;
    err = wait_for_sync(sync, LOGSTREAM_SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "storage_read_sync timeout on '%s'", path);
    }
    return err;
}

static esp_err_t storage_delete_sync(storage_worker_t* worker, const char* path, op_sync_t* sync) {
    if (!worker || !path || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    esp_err_t err = storage_delete(worker, path, (void*)sync);
    if (err != ESP_OK) return err;
    err = wait_for_sync(sync, LOGSTREAM_SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "storage_delete_sync timeout on '%s'", path);
    }
    return err;
}

static esp_err_t storage_mkdir_sync(storage_worker_t* worker, const char* path, bool recursive, op_sync_t* sync) {
    if (!worker || !path || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    esp_err_t err = storage_mkdir(worker, path, recursive, (void*)sync);
    if (err != ESP_OK) return err;
    err = wait_for_sync(sync, LOGSTREAM_SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "storage_mkdir_sync timeout on '%s'", path);
    }
    return err;
}

// Metadata operations
static esp_err_t load_metadata(storage_worker_t* worker, const char* meta_path, logstream_meta_t* meta,
                               op_sync_t* sync_read) {
    uint8_t buffer[sizeof(logstream_meta_t)];

    esp_err_t err = storage_read_sync(worker, meta_path, buffer, sizeof(buffer), sync_read);
    if (err != ESP_OK) {
        // Initialize new metadata if file doesn't exist
        meta->version = METADATA_VERSION;
        meta->num_unread_entries = 0;
        meta->head.file_index = 0;
        meta->head.offset = 0;
        meta->tail.file_index = 0;
        meta->tail.offset = 0;
        meta->crc8 = calculate_meta_crc(meta);
        return ESP_ERR_NOT_FOUND;
    }

    // Copy and validate
    memcpy(meta, buffer, sizeof(logstream_meta_t));
    uint8_t expected_crc = calculate_meta_crc(meta);
    if (meta->crc8 != expected_crc) {
        ESP_LOGW(TAG, "Metadata CRC mismatch, reinitializing");
        meta->version = METADATA_VERSION;
        meta->num_unread_entries = 0;
        meta->head.file_index = 0;
        meta->head.offset = 0;
        meta->tail.file_index = 0;
        meta->tail.offset = 0;
        meta->crc8 = calculate_meta_crc(meta);
        return ESP_ERR_INVALID_CRC;
    }

    // Invariant: empty stream implies tail == head.
    if (meta->num_unread_entries == 0) {
        meta->tail = meta->head;
    }

    return ESP_OK;
}

static void reset_stream_files(logstream_t* stream) {
    // Delete all log files in the circular buffer
    for (uint16_t i = 0; i < MAX_FILES_PER_STREAM; ++i) {
        char path[STORAGE_MAX_PATH];
        get_file_path(stream, i, path, sizeof(path));
        (void)storage_delete_sync(&stream->logger->storage_handle, path, &stream->sync_delete);
    }
}

static esp_err_t save_metadata(storage_worker_t* worker, const char* meta_path, logstream_meta_t* meta,
                               op_sync_t* sync_write) {
    meta->crc8 = calculate_meta_crc(meta);
    uint8_t buffer[sizeof(logstream_meta_t)];
    memcpy(buffer, meta, sizeof(logstream_meta_t));
    return storage_write_sync(worker, meta_path, buffer, sizeof(buffer), sync_write);
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

esp_err_t logstream_open(logger_t* logger, const char* stream_name, logstream_t* out_stream) {
    if (!logger || !logger->initialized || !stream_name || !out_stream) {
        return ESP_ERR_INVALID_ARG;
    }

    // Prepare logstream structure
    strncpy(out_stream->name, stream_name, sizeof(out_stream->name));
    out_stream->name[sizeof(out_stream->name) - 1] = '\0';

    snprintf(out_stream->dirpath, sizeof(out_stream->dirpath), "%s/%s", logger->storage_handle.mount.base_path,
             stream_name);
    out_stream->logger = logger;

    // Create a single semaphore used for all sync contexts
    out_stream->op_sem = xSemaphoreCreateBinary();
    if (out_stream->op_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore for stream '%s'", stream_name);
        return ESP_ERR_NO_MEM;
    }
    // Create mutex to protect metadata from concurrent access
    out_stream->meta_mutex = xSemaphoreCreateMutex();
    if (out_stream->meta_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex for stream '%s'", stream_name);
        vSemaphoreDelete(out_stream->op_sem);
        return ESP_ERR_NO_MEM;
    }
    // Bind semaphore to per-op contexts
    out_stream->sync_read.sem = out_stream->op_sem;
    out_stream->sync_write.sem = out_stream->op_sem;
    out_stream->sync_append.sem = out_stream->op_sem;
    out_stream->sync_delete.sem = out_stream->op_sem;
    out_stream->sync_mkdir.sem = out_stream->op_sem;

    // Create directory if needed (synchronous)
    esp_err_t err = storage_mkdir_sync(&logger->storage_handle, out_stream->dirpath, true, &out_stream->sync_mkdir);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create logstream directory '%s': %s", out_stream->dirpath, esp_err_to_name(err));
        return err;
    }

    // Load metadata
    char meta_path[STORAGE_MAX_PATH];
    get_meta_path(out_stream, meta_path, sizeof(meta_path));
    err = load_metadata(&logger->storage_handle, meta_path, &out_stream->meta, &out_stream->sync_read);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC) {
        // Fresh or invalid metadata: wipe old files to avoid format mismatches
        reset_stream_files(out_stream);
        // Persist the reset metadata
        (void)save_metadata(&logger->storage_handle, meta_path, &out_stream->meta, &out_stream->sync_write);
        ESP_LOGI(TAG, "Initialized new metadata for stream '%s'", stream_name);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load metadata for stream '%s'", stream_name);
        return err;
    }

    return ESP_OK;
}

esp_err_t logstream_close(logstream_t* stream) {
    if (!stream || !stream->logger) {
        return ESP_ERR_INVALID_ARG;
    }

    // Save metadata before closing
    char meta_path[STORAGE_MAX_PATH];
    get_meta_path(stream, meta_path, sizeof(meta_path));
    esp_err_t err = save_metadata(&stream->logger->storage_handle, meta_path, &stream->meta, &stream->sync_write);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save metadata for stream '%s'", stream->name);
    }

    if (stream->op_sem) {
        vSemaphoreDelete(stream->op_sem);
        stream->op_sem = NULL;
    }

    if (stream->meta_mutex) {
        vSemaphoreDelete(stream->meta_mutex);
        stream->meta_mutex = NULL;
    }

    return ESP_OK;
}

esp_err_t logstream_put(logstream_t* stream, const uint8_t* payload, size_t len) {
    if (!stream || !stream->logger || !payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Account for null terminator stored with every payload
    size_t max_payload = (STORAGE_MAX_WRITE_SIZE - ENTRY_HEADER_SIZE);
    if ((len + 1) > max_payload) {
        ESP_LOGE(TAG, "Payload too large: %u bytes (+1 for terminator, max: %d)", (unsigned)len, (int)max_payload);
        return ESP_ERR_INVALID_SIZE;
    }

    // Calculate total entry size (header + payload with terminator)
    size_t total_entry_size = ENTRY_HEADER_SIZE + (len + 1);

    // Protect metadata access from concurrent producers
    xSemaphoreTake(stream->meta_mutex, portMAX_DELAY);

    // Check if buffer is full.
    // Full means: writing this entry would overwrite unread data (tail) in the circular address space.
    if (stream->meta.num_unread_entries != 0) {
        // Compute where we would start writing (may wrap to next file if it doesn't fit).
        logstream_pointer_t write_start = stream->meta.head;
        if (write_start.offset + total_entry_size > MAX_FILE_SIZE) {
            write_start.file_index = (write_start.file_index + 1) % MAX_FILES_PER_STREAM;
            write_start.offset = 0;
        }

        // Proper ring-buffer full detection (option B): when moving forward from write_start,
        // the tail must not appear within the bytes we are about to write.
        uint32_t dist_to_tail = ring_forward_distance_bytes(&write_start, &stream->meta.tail);
        if (dist_to_tail < total_entry_size) {
            ESP_LOGW(TAG, "Buffer full, dropping entry (head %u:%lu tail %u:%lu)", stream->meta.head.file_index,
                     stream->meta.head.offset, stream->meta.tail.file_index, stream->meta.tail.offset);
            xSemaphoreGive(stream->meta_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Prepare entry with header
    uint8_t entry_buffer[STORAGE_MAX_WRITE_SIZE];
    log_entry_header_t* header = (log_entry_header_t*)entry_buffer;
    // Write payload and null terminator
    memcpy(entry_buffer + ENTRY_HEADER_SIZE, payload, len);
    entry_buffer[ENTRY_HEADER_SIZE + len] = '\0';

    header->length = (uint16_t)(len + 1);  // include terminator
    header->checksum = calculate_entry_checksum(entry_buffer + ENTRY_HEADER_SIZE, header->length);

    // Check if entry fits in current file
    if (stream->meta.head.offset + total_entry_size > MAX_FILE_SIZE) {
        // Move to next file
        stream->meta.head.file_index = (stream->meta.head.file_index + 1) % MAX_FILES_PER_STREAM;
        stream->meta.head.offset = 0;
    }

    // Write entry to current file
    char file_path[STORAGE_MAX_PATH];
    get_file_path(stream, stream->meta.head.file_index, file_path, sizeof(file_path));

    esp_err_t err;
    if (stream->meta.head.offset == 0) {
        // Write new file
        err = storage_write_sync(&stream->logger->storage_handle, file_path, entry_buffer, total_entry_size,
                                 &stream->sync_write);
    } else {
        // Append to existing file
        err = storage_append_sync(&stream->logger->storage_handle, file_path, entry_buffer, total_entry_size,
                                  &stream->sync_append);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
        xSemaphoreGive(stream->meta_mutex);
        return err;
    }

    // Update metadata
    stream->meta.head.offset += total_entry_size;
    stream->meta.num_unread_entries++;

    // Save metadata
    char meta_path[STORAGE_MAX_PATH];
    get_meta_path(stream, meta_path, sizeof(meta_path));
    err = save_metadata(&stream->logger->storage_handle, meta_path, &stream->meta, &stream->sync_write);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save metadata");
    }

    xSemaphoreGive(stream->meta_mutex);
    return ESP_OK;
}

esp_err_t logstream_get_unread(logstream_t* stream, uint8_t* out, size_t out_size, size_t* bytes_read) {
    if (!stream || !stream->logger || !out || !bytes_read) {
        return ESP_ERR_INVALID_ARG;
    }

    *bytes_read = 0;
    xSemaphoreTake(stream->meta_mutex, portMAX_DELAY);

    if (stream->meta.num_unread_entries == 0) {
        xSemaphoreGive(stream->meta_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    logstream_pointer_t current_pos = stream->meta.tail;
    size_t out_offset = 0;
    uint32_t entries_read = 0;
    uint8_t read_buffer[STORAGE_MAX_WRITE_SIZE];
    uint16_t current_file = current_pos.file_index;
    bool file_loaded = false;

    // Read entries until buffer full or no more unread entries
    while (entries_read < stream->meta.num_unread_entries && out_offset < out_size) {
        // Load new file if needed
        if (!file_loaded || current_file != current_pos.file_index) {
            char file_path[STORAGE_MAX_PATH];
            get_file_path(stream, current_pos.file_index, file_path, sizeof(file_path));

            esp_err_t err = storage_read_sync(&stream->logger->storage_handle, file_path, read_buffer,
                                              sizeof(read_buffer), &stream->sync_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read file %u: %s", current_pos.file_index, esp_err_to_name(err));
                break;
            }
            current_file = current_pos.file_index;
            file_loaded = true;
        }

        // Check if we have enough space for header
        if (current_pos.offset + ENTRY_HEADER_SIZE > MAX_FILE_SIZE) {
            // Move to next file
            current_pos.file_index = (current_pos.file_index + 1) % MAX_FILES_PER_STREAM;
            current_pos.offset = 0;
            file_loaded = false;
            continue;
        }

        // Parse header
        log_entry_header_t* header = (log_entry_header_t*)(read_buffer + current_pos.offset);
        uint16_t payload_len = header->length;

        // Check for EOF marker (zero length) or invalid length
        if (payload_len == 0 || payload_len > (MAX_FILE_SIZE - ENTRY_HEADER_SIZE)) {
            // End of valid data in this file, move to next
            current_pos.file_index = (current_pos.file_index + 1) % MAX_FILES_PER_STREAM;
            current_pos.offset = 0;
            file_loaded = false;
            continue;
        }

        // Check if entry fits in current file
        if (current_pos.offset + ENTRY_HEADER_SIZE + payload_len > MAX_FILE_SIZE) {
            ESP_LOGW(TAG, "Entry exceeds file boundary at file %u offset %lu; moving to next file",
                     current_pos.file_index, current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % MAX_FILES_PER_STREAM;
            current_pos.offset = 0;
            file_loaded = false;
            continue;
        }

        // Check if entry fits in output buffer
        if (out_offset + payload_len > out_size) {
            // Buffer full, stop here
            break;
        }

        // Verify checksum
        uint8_t* payload_ptr = read_buffer + current_pos.offset + ENTRY_HEADER_SIZE;
        uint16_t calculated_checksum = calculate_entry_checksum(payload_ptr, payload_len);
        if (calculated_checksum != header->checksum) {
            ESP_LOGW(TAG, "Checksum mismatch at file %u offset %lu; moving to next file", current_pos.file_index,
                     current_pos.offset);
            // End of valid data in this file, move to next
            current_pos.file_index = (current_pos.file_index + 1) % MAX_FILES_PER_STREAM;
            current_pos.offset = 0;
            file_loaded = false;
            continue;
        }

        // Copy payload to output buffer
        memcpy(out + out_offset, payload_ptr, payload_len);
        out_offset += payload_len;
        entries_read++;

        // Advance position
        current_pos.offset += ENTRY_HEADER_SIZE + payload_len;
        if (current_pos.offset >= MAX_FILE_SIZE) {
            current_pos.file_index = (current_pos.file_index + 1) % MAX_FILES_PER_STREAM;
            current_pos.offset = 0;
            file_loaded = false;
        }

        // Check if we've reached the head pointer (stop after advancing, not before)
        if (current_pos.file_index == stream->meta.head.file_index && current_pos.offset >= stream->meta.head.offset) {
            // Reached the head, stop reading
            break;
        }
    }

    *bytes_read = out_offset;

    // Update metadata if we read any entries
    if (entries_read > 0) {
        stream->meta.tail = current_pos;
        stream->meta.num_unread_entries -= entries_read;

        // Invariant: empty stream implies tail == head.
        if (stream->meta.num_unread_entries == 0) {
            stream->meta.tail = stream->meta.head;
        }

        // Save metadata
        char meta_path[STORAGE_MAX_PATH];
        get_meta_path(stream, meta_path, sizeof(meta_path));
        esp_err_t err = save_metadata(&stream->logger->storage_handle, meta_path, &stream->meta, &stream->sync_write);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save metadata");
        }
    }

    xSemaphoreGive(stream->meta_mutex);
    return (entries_read > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t logstream_get_status(logstream_t* stream, logstream_meta_t* out_meta) {
    if (!stream || !out_meta) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out_meta, &stream->meta, sizeof(logstream_meta_t));
    return ESP_OK;
}