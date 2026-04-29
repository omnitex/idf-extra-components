idf_component_register(INCLUDE_DIRS dhara
                       SRC_DIRS "dhara/dhara")

# Forward Kconfig value as a compile definition so journal.h can use it.
# Both the dhara component (journal.c) and consumers (dhara_glue.c) need
# the same value to keep struct dhara_journal layout consistent.
if(DEFINED CONFIG_DHARA_META_CACHE_SLOTS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_META_CACHE_SLOTS=${CONFIG_DHARA_META_CACHE_SLOTS})
endif()
