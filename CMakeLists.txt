cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND SHIELD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(kerfur)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
    src/main.c
    src/app/app.c
    src/core/event_bus.c
    src/behavior/behavior_engine.c
    src/ui/ui_renderer.c
    src/drivers/mock_inputs.c
    src/power/power_manager.c
    src/ble/ble_manager.c
)
