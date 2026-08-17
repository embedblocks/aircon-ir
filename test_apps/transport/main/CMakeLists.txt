idf_component_register(
    SRCS "test_app_main.c"
    INCLUDE_DIRS "."
    REQUIRES 13_ac_control driver esp_timer
)
