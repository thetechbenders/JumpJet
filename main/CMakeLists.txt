idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES jj_authority jj_control jj_identity jj_interlock jj_portal dc_evlog dc_prusa dc_wifi nvs_flash app_update
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
