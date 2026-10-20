# SPIFFS

[![Component Registry](https://components.espressif.com/components/espressif/spiffs/badge.svg)](https://components.espressif.com/components/espressif/spiffs)

SPIFFS file system for SPI NOR flash, with ESP-IDF VFS integration (`esp_vfs_spiffs_register()`) and the `spiffs_create_partition_image()` CMake function for build-time images.

The file system core comes from [pellepl/spiffs](https://github.com/pellepl/spiffs), pinned as a git submodule. This component was previously part of ESP-IDF as `components/spiffs`.

New designs should use [LittleFS](https://components.espressif.com/components/joltwallet/littlefs) instead. It survives power loss during writes, has dynamic wear levelling and directories, and supports flash encryption. See [File System Considerations](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/file-system-considerations.html).

## Using the component

```
idf.py add-dependency espressif/spiffs
```

The API, the `CONFIG_SPIFFS_*` options, and the on-flash format are the same as in the ESP-IDF version, so existing images keep mounting.
