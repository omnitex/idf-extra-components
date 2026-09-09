idf_component_register(INCLUDE_DIRS dhara
                       SRC_DIRS "dhara/dhara")

if(CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_PATH_CACHE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        DHARA_MAP_PATH_CACHE=0)
endif()
