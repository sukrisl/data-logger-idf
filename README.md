# Data Logger IDF Component

A circular buffer-based data logger for ESP-IDF v5.4 that provides persistent storage of log entries using the data_storage_idf component.

## Features

- **No Dynamic Allocation**: All buffers are stack-allocated
- **Minimal Global Variables**: State is maintained in user-provided structures
- **Circular Buffer**: Uses up to 16 files per stream with automatic wraparound
- **Persistent Metadata**: CRC-validated metadata tracks read/unread entries
- **Entry Integrity**: Each log entry has a checksum for validation
- **Asynchronous Storage**: Uses data_storage_idf worker for non-blocking operations

## Architecture

### File Structure per Stream
```
/littlefs/stream_name/
├── meta              # Stream metadata (CRC-protected)
├── 0.log             # Log file 0
├── 1.log             # Log file 1
└── ...up to 15.log   # Log file 15 (circular)
```

### Constants
- `MAX_FILES_PER_STREAM`: 16 files per stream
- `MAX_FILE_SIZE`: 4096 bytes per file
- `STORAGE_MAX_WRITE_SIZE`: 512 bytes (maximum entry size)
- `ENTRY_HEADER_SIZE`: 4 bytes (length + checksum)

### Log Entry Format
```
[2 bytes: length][2 bytes: checksum][payload data...]
```

## API Usage

### Initialize Logger
```c
mount_point_t mount = {
    .base_path = "/littlefs",
    .partition_label = "storage",
    .auto_format_on_fail = true
};

logger_t logger = {0};
esp_err_t err = logger_init(mount, &logger);
```

### Open a Log Stream
```c
logstream_t stream = {0};
err = logstream_open(&logger, "sensor_data", &stream);
```

### Write Log Entries
```c
const char* data = "Temperature: 25.5C";
err = logstream_put(&stream, (uint8_t*)data, strlen(data));
```

### Read Unread Entries
```c
uint8_t buffer[128];
logstream_pointer_t read_ptr;

err = logstream_get_unread(&stream, buffer, sizeof(buffer), &read_ptr);
if (err == ESP_OK) {
    // Process the data
    printf("Read: %s\n", (char*)buffer);
    
    // Mark as read
    logstream_mark_read(&stream, read_ptr);
}
```

### Get Stream Status
```c
logstream_meta_t status;
logstream_get_status(&stream, &status);
printf("Unread entries: %lu\n", status.num_unread_entries);
```

### Close Stream and Deinitialize
```c
logstream_close(&stream);
logger_deinit(&logger);
```

## Circular Buffer Behavior

1. **New entries** are written at the `head` pointer
2. **Unread entries** are read from the `tail` pointer
3. When a file fills up, the head moves to the next file (0-15, then wraps to 0)
4. If head catches up to tail (buffer full), old files are deleted and tail advances
5. Metadata persists across reboots to maintain unread entry tracking

## Memory Usage

- **No heap allocation**: All buffers are stack-based
- **Per-operation stack usage**:
  - Write: ~520 bytes (entry buffer + paths)
  - Read: ~520 bytes (read buffer + paths)
  - Metadata: ~30 bytes structure
- **Persistent storage**: ~4KB per file × 16 files = 64KB per stream maximum

## Error Handling

- `ESP_ERR_INVALID_ARG`: Null pointer or invalid parameters
- `ESP_ERR_INVALID_SIZE`: Payload exceeds maximum size
- `ESP_ERR_NOT_FOUND`: No unread entries available
- `ESP_ERR_INVALID_CRC`: Entry checksum validation failed
- `ESP_ERR_INVALID_STATE`: Operation not valid in current state

## Dependencies

- **data_storage_idf**: Asynchronous file operations
- **esp_timer**: Timer functionality
- **esp_crc**: CRC calculation for metadata

## Thread Safety

- Operations are serialized through the data_storage_idf worker task
- Multiple streams can be used concurrently from different tasks
- Each logstream_t should be accessed from a single task or protected by mutex

## Example

See [main/main.c](../../main/main.c) for a complete example demonstrating:
- Logger initialization
- Stream creation
- Writing log entries
- Reading unread entries
- Proper cleanup
