#include "logstream.h"
#include <string.h>
#include "esp_crc.h"
#include "esp_log.h"

#define LOGGER_TIMEOUT_MS 1000

#define ENTRY_HEADER_SIZE 4

#define METADATA_VERSION 1

static const char* TAG = "logstream";

// Log entry header structure (4 bytes)
typedef struct __attribute__((packed)) {
    uint16_t length;    // Payload length
    uint16_t checksum;  // Simple checksum
} log_entry_header_t;

// Helper functions
static uint8_t calculate_meta_crc(const logstream_meta_t* meta) {
    return esp_crc8_le(0, (const uint8_t*)meta, sizeof(*meta) - sizeof(meta->crc8));
}

static uint16_t calculate_entry_checksum(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static inline uint32_t ring_total_capacity_bytes(const logstream_t* stream) {
    return (uint32_t)(stream->max_num_files * stream->max_file_size);
}

static inline uint32_t ring_abs_bytes(const logstream_pointer_t* ptr, const logstream_t* stream) {
    return (uint32_t)((ptr->file_index * stream->max_file_size) + ptr->offset);
}

// Forward distance from 'from' to 'to' when moving forward in the ring.
static inline uint32_t ring_forward_distance_bytes(const logstream_pointer_t* from, const logstream_pointer_t* to,
                                                   const logstream_t* stream) {
    uint32_t total = ring_total_capacity_bytes(stream);
    uint32_t a = ring_abs_bytes(from, stream);
    uint32_t b = ring_abs_bytes(to, stream);
    return (b + total - a) % total;
}

static void get_file_path(const logstream_t* stream, uint16_t file_index, char* out_path, size_t max_len) {
    snprintf(out_path, max_len, "%s/%u.log", stream->dirpath, file_index);
}

// Synchronous wrappers around storage_worker operations
static inline void sync_prepare(op_sync_t* sync) {
    if (!sync || !sync->sem) return;
    while (xSemaphoreTake(sync->sem, 0) == pdTRUE) {
    }
    sync->completed = false;
    sync->status = ESP_OK;
    sync->bytes_processed = 0;
    sync->read_len = 0;
}

static inline esp_err_t wait_for_sync(op_sync_t* sync, uint32_t timeout_ms) {
    if (!sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(sync->sem, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return sync->status;
}

static inline esp_err_t wait_for_sync_or_log_timeout(op_sync_t* sync, const char* op_name, const char* path) {
    esp_err_t err = wait_for_sync(sync, LOGGER_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "%s timeout on '%s'", op_name ? op_name : "storage_op", path ? path : "(null)");
    }
    return err;
}

static esp_err_t storage_write_sync(storage_worker_t* worker, const char* path, const void* data, size_t len,
                                    op_sync_t* sync) {
    if (!worker || !path || !data || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_write(worker, path, data, len, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_write", path);
}

static esp_err_t storage_append_sync(storage_worker_t* worker, const char* path, const void* data, size_t len,
                                     op_sync_t* sync) {
    if (!worker || !path || !data || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_append(worker, path, data, len, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_append", path);
}

static esp_err_t storage_read_sync(storage_worker_t* worker, const char* path, void* buf, size_t buf_size,
                                   op_sync_t* sync) {
    if (!worker || !path || !buf || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_read(worker, path, buf, buf_size, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_read", path);
}

static esp_err_t storage_read_at_sync(storage_worker_t* worker, const char* path, uint32_t offset, void* buf,
                                      size_t buf_size, op_sync_t* sync) {
    if (!worker || !path || !buf || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_read_at(worker, path, offset, buf, buf_size, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_read_at", path);
}

static esp_err_t storage_delete_sync(storage_worker_t* worker, const char* path, op_sync_t* sync) {
    if (!worker || !path || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_delete(worker, path, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_delete", path);
}

static esp_err_t storage_mkdir_sync(storage_worker_t* worker, const char* path, bool recursive, op_sync_t* sync) {
    if (!worker || !path || !sync || !sync->sem) return ESP_ERR_INVALID_ARG;
    sync_prepare(sync);
    esp_err_t err = storage_mkdir(worker, path, recursive, (void*)sync);
    if (err != ESP_OK) return err;
    return wait_for_sync_or_log_timeout(sync, "storage_mkdir", path);
}

// Metadata operations
static esp_err_t load_metadata(logstream_t* stream) {
    uint8_t buffer[sizeof(logstream_meta_t)];

    storage_worker_t* worker = &stream->logger->storage_handle;
    if (!worker) return ESP_ERR_INVALID_ARG;

    logstream_meta_t* meta = &stream->meta;
    if (!meta) return ESP_ERR_INVALID_ARG;

    esp_err_t err = storage_read_sync(worker, stream->metapath, buffer, sizeof(buffer), &stream->sync_read);
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

    // // Invariant: empty stream implies tail == head.
    // if (meta->num_unread_entries == 0) {
    //     meta->tail = meta->head;
    // }

    return ESP_OK;
}

static void reset_stream_files(logstream_t* stream) {
    // Delete all log files in the circular buffer
    for (uint16_t i = 0; i < stream->max_num_files; ++i) {
        char path[STORAGE_MAX_PATH];
        get_file_path(stream, i, path, sizeof(path));
        esp_err_t err = storage_delete_sync(&stream->logger->storage_handle, path, &stream->sync_delete);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to delete log file '%s' during reset: %s", path, esp_err_to_name(err));
        }
    }
}

static esp_err_t save_metadata(logstream_t* stream) {
    if (!stream || !stream->logger) return ESP_ERR_INVALID_ARG;

    logstream_meta_t* meta = &stream->meta;
    storage_worker_t* worker = &stream->logger->storage_handle;

    meta->crc8 = calculate_meta_crc(meta);
    uint8_t buffer[sizeof(logstream_meta_t)];
    memcpy(buffer, meta, sizeof(logstream_meta_t));
    return storage_write_sync(worker, stream->metapath, buffer, sizeof(buffer), &stream->sync_write);
}

static esp_err_t commit_metadata(logstream_t* stream, const logstream_meta_t new_meta) {
    if (!stream) return ESP_ERR_INVALID_ARG;

    logstream_meta_t old_meta = stream->meta;
    stream->meta = new_meta;

    esp_err_t err = save_metadata(stream);

    // Restore old metadata on failure
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit metadata changes, reverting");
        stream->meta = old_meta;
    }
    return err;
}

static esp_err_t check_buffer_capacity(logstream_t* stream, size_t entry_size) {
    if (!stream) return ESP_ERR_INVALID_ARG;

    // If there are no unread entries, there is always space.
    if (stream->meta.num_unread_entries == 0) return ESP_OK;

    // Compute where we would start writing (may wrap to next file if it doesn't fit).
    logstream_pointer_t write_start = stream->meta.head;
    if (write_start.offset + entry_size > stream->max_file_size) {
        write_start.file_index = (write_start.file_index + 1) % stream->max_num_files;
        write_start.offset = 0;
    }

    // When moving forward from write_start, the tail must not appear within the bytes we are about to write.
    uint32_t dist_to_tail = ring_forward_distance_bytes(&write_start, &stream->meta.tail, stream);
    if (dist_to_tail >= entry_size) return ESP_OK;

    ESP_LOGW(TAG, "Buffer full, dropping entry (head %u:%lu tail %u:%lu)", stream->meta.head.file_index,
             stream->meta.head.offset, stream->meta.tail.file_index, stream->meta.tail.offset);
    return ESP_ERR_NO_MEM;
}

esp_err_t logstream_open(logger_t* logger, const char* stream_name, logstream_t* out_stream, uint16_t max_num_files,
                         uint16_t max_file_size) {
    if (!logger || !logger->initialized || !stream_name || !out_stream) return ESP_ERR_INVALID_ARG;

    if (strlen(stream_name) >= sizeof(out_stream->name)) {
        ESP_LOGE(TAG, "Stream name too long: %s. Max length is %d", stream_name, (int)sizeof(out_stream->name) - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    // Prepare logstream structure
    snprintf(out_stream->name, sizeof(out_stream->name), "%s", stream_name);
    snprintf(out_stream->dirpath, sizeof(out_stream->dirpath), "/%s", stream_name);
    snprintf(out_stream->metapath, sizeof(out_stream->metapath), "%s/meta.bin", (const char*)out_stream->dirpath);
    out_stream->logger = logger;
    out_stream->max_num_files = max_num_files;
    out_stream->max_file_size = max_file_size;

    out_stream->op_sem = xSemaphoreCreateBinaryStatic(&out_stream->op_sem_storage);
    out_stream->meta_mutex = xSemaphoreCreateMutexStatic(&out_stream->meta_mutex_storage);
    if (out_stream->op_sem == NULL || out_stream->meta_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sync primitives for stream '%s'", stream_name);
        return ESP_ERR_NO_MEM;
    }
    // Bind semaphore to per-op contexts
    out_stream->sync_read.sem = out_stream->op_sem;
    out_stream->sync_read_at.sem = out_stream->op_sem;
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
    err = load_metadata(out_stream);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC) {
        reset_stream_files(out_stream);   // Fresh or invalid metadata: wipe old files to avoid format mismatches
        err = save_metadata(out_stream);  // Persist the reset metadata
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save new metadata for stream '%s'", stream_name);
            return err;
        }
        ESP_LOGI(TAG, "Initialized new metadata for stream '%s'", stream_name);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load metadata for stream '%s'", stream_name);
        return err;
    }

    ESP_LOGI(TAG, "Opened logstream '%s'", stream_name);
    return ESP_OK;
}

esp_err_t logstream_close(logstream_t* stream) {
    if (!stream || !stream->logger) return ESP_ERR_INVALID_ARG;

    // Save metadata before closing
    esp_err_t err = save_metadata(stream);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save metadata for stream '%s', error=%s", stream->name, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t logstream_put(logstream_t* stream, const uint8_t* payload, size_t len) {
    if (!stream || !stream->logger || !payload || len == 0) return ESP_ERR_INVALID_ARG;

    // Account for null terminator stored with every payload
    size_t max_payload = (STORAGE_MAX_WRITE_SIZE - ENTRY_HEADER_SIZE);
    if (len > max_payload) {
        ESP_LOGE(TAG, "Payload too large: %u bytes (+1 for terminator, max: %d)", (unsigned)len, (int)max_payload);
        return ESP_ERR_INVALID_SIZE;
    }

    // Calculate total entry size (header + payload with terminator)
    size_t total_entry_size = ENTRY_HEADER_SIZE + len;

    // Protect metadata access from concurrent producers
    if (xSemaphoreTake(stream->meta_mutex, pdMS_TO_TICKS(LOGGER_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Another operation is in progress on stream '%s'", stream->name);
        return ESP_ERR_INVALID_STATE;
    }

    // Check if buffer is full.
    // Full means: writing this entry would overwrite unread data (tail) in the circular address space.
    esp_err_t err = check_buffer_capacity(stream, total_entry_size);
    if (err != ESP_OK) {
        xSemaphoreGive(stream->meta_mutex);
        return err;
    }

    // Prepare entry with header
    uint8_t entry_buffer[total_entry_size];
    log_entry_header_t* header = (log_entry_header_t*)entry_buffer;
    // Write payload and null terminator
    memcpy(entry_buffer + ENTRY_HEADER_SIZE, payload, len);
    entry_buffer[total_entry_size - 1] = '\0';

    header->length = (uint16_t)len;
    header->checksum = calculate_entry_checksum(entry_buffer + ENTRY_HEADER_SIZE, header->length);

    logstream_meta_t meta_temp = stream->meta;

    // Check if entry fits in current file
    if (meta_temp.head.offset + total_entry_size > stream->max_file_size) {
        ESP_LOGW(
            TAG,
            "Log entry does not fit in current file %u at offset %lu (size %u) [offset after %lu], moving to next file",
            meta_temp.head.file_index, meta_temp.head.offset, total_entry_size,
            meta_temp.head.offset + total_entry_size);
        // Move to next file
        meta_temp.head.file_index = (meta_temp.head.file_index + 1) % stream->max_num_files;

        if (meta_temp.head.file_index != meta_temp.tail.file_index) {
            char file_path[STORAGE_MAX_PATH];
            get_file_path(stream, meta_temp.head.file_index, file_path, sizeof(file_path));

            char erase_buffer[stream->max_file_size];
            memset(erase_buffer, 0xFF, sizeof(erase_buffer));
            // Pre-erase next file
            err = storage_write_sync(&stream->logger->storage_handle, file_path, erase_buffer, sizeof(erase_buffer),
                                     &stream->sync_write);
            meta_temp.head.offset = 0;
        } else {
            ESP_LOGW(TAG, "Buffer full when trying to advance to next file, dropping entry");
            xSemaphoreGive(stream->meta_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Write entry to current file
    char file_path[STORAGE_MAX_PATH];
    get_file_path(stream, meta_temp.head.file_index, file_path, sizeof(file_path));

    // Write new file if offset is zero, else append
    ESP_LOGW(TAG, "Writing log entry to %s at offset %lu", file_path, meta_temp.head.offset);
    if (meta_temp.head.offset == 0) {
        err = storage_write_sync(&stream->logger->storage_handle, file_path, entry_buffer, total_entry_size,
                                 &stream->sync_write);
    } else {
        err = storage_append_sync(&stream->logger->storage_handle, file_path, entry_buffer, total_entry_size,
                                  &stream->sync_append);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
        xSemaphoreGive(stream->meta_mutex);
        return err;
    }

    // Update metadata
    meta_temp.head.offset += total_entry_size;
    meta_temp.num_unread_entries++;

    // Save metadata
    err = commit_metadata(stream, meta_temp);

    xSemaphoreGive(stream->meta_mutex);
    return err;
}

esp_err_t logstream_get_unread(logstream_t* stream, uint8_t* out, size_t out_size, size_t* bytes_read) {
    if (!stream || !stream->logger || !out || !bytes_read) {
        return ESP_ERR_INVALID_ARG;
    }

    *bytes_read = 0;
    if (xSemaphoreTake(stream->meta_mutex, pdMS_TO_TICKS(LOGGER_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Another operation is in progress on stream '%s'", stream->name);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    // No unread entries
    if (stream->meta.num_unread_entries == 0) {
        err = ESP_ERR_NOT_FOUND;
        xSemaphoreGive(stream->meta_mutex);
        return err;
    }

    logstream_pointer_t current_pos = stream->meta.tail;
    size_t out_offset = 0;
    uint32_t entries_read = 0;
    char file_path[STORAGE_MAX_PATH];

    // Read entries until buffer full or no more unread entries
    while (entries_read < stream->meta.num_unread_entries && out_offset < out_size) {
        get_file_path(stream, current_pos.file_index, file_path, sizeof(file_path));

        // Read header at current_pos.offset
        log_entry_header_t header;
        ESP_LOGW(TAG, "Reading log entry header from %s at offset %lu", file_path, current_pos.offset);
        err = storage_read_at_sync(&stream->logger->storage_handle, file_path, current_pos.offset, &header,
                                   sizeof(header), &stream->sync_read_at);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: Failed to read header from %s offset %lu: %s", stream->name, file_path,
                     current_pos.offset, esp_err_to_name(err));
            xSemaphoreGive(stream->meta_mutex);
            return err;
        }

        if (stream->sync_read_at.bytes_processed == 0) {
            // Empty file or EOF at this offset; move to next file
            ESP_LOGW(TAG, "%s: Reached EOF at %s offset %lu; moving to next file", stream->name, file_path,
                     current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
            continue;
        }

        if (stream->sync_read_at.read_len < sizeof(header)) {
            ESP_LOGW(TAG, "%s: Incomplete header at %s offset %lu; moving to next file", stream->name, file_path,
                     current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
            continue;
        }

        uint16_t payload_len = header.length;

        // Check for EOF marker (zero length) or invalid length
        if (payload_len == 0 || payload_len >= (stream->max_file_size - ENTRY_HEADER_SIZE)) {
            // End of valid data in this file, move to next
            ESP_LOGW(TAG, "%s: Invalid or zero-length entry at file %u offset %lu; moving to next file", stream->name,
                     current_pos.file_index, current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
            continue;
        }

        // Check if entry fits in output buffer
        if (out_offset + payload_len > out_size) {
            // Buffer full, stop here
            break;
        }

        // Read payload at current_pos.offset + header
        err = storage_read_at_sync(&stream->logger->storage_handle, file_path, current_pos.offset + ENTRY_HEADER_SIZE,
                                   out + out_offset, payload_len, &stream->sync_read_at);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: Failed to read payload from %s offset %lu len %u: %s", stream->name, file_path,
                     current_pos.offset + ENTRY_HEADER_SIZE, (unsigned)payload_len, esp_err_to_name(err));
            xSemaphoreGive(stream->meta_mutex);
            return err;
        }

        if (stream->sync_read_at.read_len < payload_len) {
            ESP_LOGW(TAG, "%s: Entry exceeds file boundary at file %u offset %lu; moving to next file", stream->name,
                     current_pos.file_index, current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
            continue;
        }

        // Verify checksum over the payload bytes we just read
        uint16_t calculated_checksum = calculate_entry_checksum(out + out_offset, payload_len);
        if (calculated_checksum != header.checksum) {
            ESP_LOGW(TAG, "%s: Checksum mismatch at file %u offset %lu; moving to next file", stream->name,
                     current_pos.file_index, current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
            continue;
        }

        out_offset += payload_len;
        entries_read++;

        // Advance position
        current_pos.offset += ENTRY_HEADER_SIZE + payload_len;
        if (current_pos.offset >= stream->max_file_size) {
            ESP_LOGW(TAG, "%s: Reached end of file %u offset %lu; moving to next file", stream->name,
                     current_pos.file_index, current_pos.offset);
            current_pos.file_index = (current_pos.file_index + 1) % stream->max_num_files;
            current_pos.offset = 0;
        }

        // Check if we've reached the head pointer (stop after advancing, not before)
        if (current_pos.file_index == stream->meta.head.file_index && current_pos.offset >= stream->meta.head.offset) {
            break;  // Reached the head, stop reading
        }
    }

    *bytes_read = out_offset;

    // Update metadata if we read any entries
    logstream_meta_t meta_temp = stream->meta;
    if (entries_read > 0) {
        meta_temp.tail = current_pos;
        meta_temp.num_unread_entries -= entries_read;

        // Invariant: empty stream implies tail == head.
        if (meta_temp.num_unread_entries == 0) {
            meta_temp.tail = meta_temp.head;
        }

        // Save metadata
        err = commit_metadata(stream, meta_temp);
        if (err != ESP_OK) {
            xSemaphoreGive(stream->meta_mutex);
            return err;
        }
    }

    err = (entries_read > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(stream->meta_mutex);
    return err;
}

esp_err_t logstream_get_status(logstream_t* stream, logstream_meta_t* out_meta) {
    if (!stream || !out_meta) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out_meta, &stream->meta, sizeof(logstream_meta_t));
    return ESP_OK;
}