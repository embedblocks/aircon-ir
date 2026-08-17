file(GLOB PROTOCOL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/protocols/*.c"
)

idf_component_register(
    SRCS
        "src/ac_operation.c"
        "src/ac_protocol_factory.c"
        "src/ac_ir.c"
        "src/protocols/gree.c"
        ${PROTOCOL_SOURCES} # Contains haier.c, midea.c ,....

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "src"

    PRIV_REQUIRES
        driver
)