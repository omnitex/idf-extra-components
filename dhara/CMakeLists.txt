idf_component_register(INCLUDE_DIRS dhara
                       SRC_DIRS "dhara/dhara")

if(CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_PATH_CACHE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_PATH_CACHE=0)
endif()

if(CONFIG_NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_EXACT_REPEAT_CACHE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_EXACT_REPEAT_CACHE=0)
endif()

# Forward the slot count as a compile definition so journal.h/journal.c can
# size struct dhara_journal accordingly. Consumers that also read the cache
# fields directly (spi_nand_flash/src/dhara_glue.c) need the exact same
# value to keep struct dhara_journal layout consistent.
if(DEFINED CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_META_CACHE_SLOTS=${CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS})
endif()
