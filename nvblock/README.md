# nvblock

ESP-IDF component wrapper for the [nvblock](https://github.com/Laczen/nvblock) library.

## Overview

nvblock is a small translation layer that provides block based access for non volatile memories (e.g. eeprom, nor-flash, nand-flash). It is designed for usage on resource constrained systems.

## Features

* **Configurable block size**: minimum of 32 bytes (or the write block size if bigger)
* **Wear levelling**: the erase of any two (non bad) blocks differ with at most 1
* **Trim support**: blocks can be deleted when no longer needed to improve performance
* **Data integrity**: write and trim operations are atomic with power-fail recovery
* **Bad block support**: optional for eeprom/NOR flash, required for NAND flash

## License

nvblock is licensed under Apache License 2.0.

## Documentation

For detailed documentation, see the [nvblock repository](https://github.com/Laczen/nvblock).
