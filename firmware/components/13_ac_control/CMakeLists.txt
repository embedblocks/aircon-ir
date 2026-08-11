idf_component_register(
    SRCS
        "src/ac_operation.c"
        "src/ac_protocol_factory.c"
        "src/ac_ir.c"
        "src/protocols/haier.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "src"
    PRIV_REQUIRES
        driver
)