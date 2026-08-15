set(ui_srcs "ui_topway.c")
set(ui_requires
    driver
    esp_timer
    ota
    core
    monitor
    drivers
    network
    app_update
    telegram
)

idf_component_register(
    SRCS ${ui_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${ui_requires}
)
