idf_component_register(
    SRCS "jj_control.c"
    INCLUDE_DIRS "include"
    REQUIRES jj_interlock dc_pid
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
