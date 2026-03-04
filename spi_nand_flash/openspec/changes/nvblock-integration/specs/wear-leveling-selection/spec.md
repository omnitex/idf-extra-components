# Wear Leveling Selection Specification

## ADDED Requirements

### Requirement: Kconfig menu for wear leveling selection
The system SHALL provide a Kconfig choice menu for selecting wear leveling implementation.

#### Scenario: User selects Dhara (default)
- **WHEN** user runs `idf.py menuconfig`
- **AND** navigates to "Component config → SPI NAND Flash configuration"
- **THEN** system SHALL display "Wear Leveling Implementation" choice menu
- **AND** "Dhara" option SHALL be selected by default
- **AND** help text SHALL describe Dhara as mature, well-tested implementation

#### Scenario: User selects nvblock
- **WHEN** user selects "nvblock" option in Kconfig menu
- **AND** saves configuration
- **THEN** CONFIG_NAND_FLASH_WEAR_LEVELING_NVBLOCK SHALL be defined
- **AND** CONFIG_NAND_FLASH_WEAR_LEVELING_DHARA SHALL NOT be defined
- **AND** help text SHALL describe nvblock features (small footprint, configurable blocks)

### Requirement: Conditional compilation based on Kconfig
The system SHALL compile only the selected wear leveling implementation.

#### Scenario: Build with Dhara selected
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_DHARA is defined
- **AND** build system processes CMakeLists.txt
- **THEN** system SHALL include src/dhara_glue.c in compilation
- **AND** system SHALL NOT include src/nvblock_glue.c
- **AND** system SHALL link dhara component dependency

#### Scenario: Build with nvblock selected
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_NVBLOCK is defined
- **AND** build system processes CMakeLists.txt
- **THEN** system SHALL include src/nvblock_glue.c in compilation
- **AND** system SHALL NOT include src/dhara_glue.c
- **AND** system SHALL link nvblock component dependency

### Requirement: Single implementation registration
The system SHALL ensure only one wear leveling implementation registers operations.

#### Scenario: nand_register_dev with Dhara
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_DHARA is defined
- **AND** `nand_register_dev()` is called from nand.c
- **THEN** dhara_glue.c's nand_register_dev SHALL be linked
- **AND** function SHALL set handle->ops to &dhara_nand_ops
- **AND** function SHALL return ESP_OK

#### Scenario: nand_register_dev with nvblock
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_NVBLOCK is defined
- **AND** `nand_register_dev()` is called from nand.c
- **THEN** nvblock_glue.c's nand_register_dev SHALL be linked
- **AND** function SHALL set handle->ops to &nvblock_nand_ops
- **AND** function SHALL return ESP_OK

### Requirement: Component dependency resolution
The system SHALL resolve component dependencies based on Kconfig selection.

#### Scenario: idf_component.yml with Dhara selected
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_DHARA is defined
- **AND** component manager resolves dependencies
- **THEN** system SHALL include dhara component
- **AND** dhara headers SHALL be available for compilation

#### Scenario: idf_component.yml with nvblock selected
- **WHEN** CONFIG_NAND_FLASH_WEAR_LEVELING_NVBLOCK is defined
- **AND** component manager resolves dependencies
- **THEN** system SHALL include nvblock component
- **AND** nvblock headers SHALL be available for compilation

### Requirement: Backward compatibility default
The system SHALL maintain Dhara as default to preserve existing behavior.

#### Scenario: Fresh project without explicit selection
- **WHEN** new project adds spi_nand_flash component
- **AND** user has not explicitly configured wear leveling
- **THEN** Dhara SHALL be selected by default
- **AND** existing projects SHALL build without configuration changes

### Requirement: Mutually exclusive selection
The system SHALL prevent simultaneous selection of both implementations.

#### Scenario: Kconfig enforces mutual exclusion
- **WHEN** user attempts to enable both Dhara and nvblock
- **THEN** Kconfig SHALL prevent this configuration
- **AND** only one option SHALL be selectable in choice menu

### Requirement: Clear documentation of selection
The system SHALL provide clear help text for each option.

#### Scenario: Dhara help text
- **WHEN** user views Dhara option help in menuconfig
- **THEN** help text SHALL describe it as "Mature FTL implementation with radix tree mapping"
- **AND** help text SHALL note it's the default and well-tested

#### Scenario: nvblock help text
- **WHEN** user views nvblock option help in menuconfig
- **THEN** help text SHALL describe it as "Lightweight implementation with configurable block sizes"
- **AND** help text SHALL note suitability for resource-constrained systems
- **AND** help text SHALL mention smaller footprint benefit

### Requirement: Configuration change handling
The system SHALL require clean build when changing wear leveling implementation.

#### Scenario: Switch from Dhara to nvblock
- **WHEN** user changes Kconfig from Dhara to nvblock
- **THEN** build system SHALL detect configuration change
- **AND** build system SHALL recommend clean build (idf.py fullclean)
- **AND** documentation SHALL warn that switching requires chip erase (incompatible data)

### Requirement: Linux target support
The system SHALL support both wear leveling implementations on Linux target.

#### Scenario: Linux build with Dhara
- **WHEN** CONFIG_IDF_TARGET_LINUX is defined
- **AND** CONFIG_NAND_FLASH_WEAR_LEVELING_DHARA is selected
- **THEN** system SHALL compile successfully with emulated NAND
- **AND** host tests SHALL use Dhara implementation

#### Scenario: Linux build with nvblock
- **WHEN** CONFIG_IDF_TARGET_LINUX is defined
- **AND** CONFIG_NAND_FLASH_WEAR_LEVELING_NVBLOCK is selected
- **THEN** system SHALL compile successfully with emulated NAND
- **AND** host tests SHALL use nvblock implementation
