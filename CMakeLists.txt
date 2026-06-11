cmake_minimum_required(VERSION 3.20.0)

# CMake 4.1 on Windows can occasionally fail to materialize the platform-info
# directory before compiler detection writes CMake*Compiler.cmake files.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CMakeFiles/${CMAKE_VERSION}")

set(KERFUR_CONF_FILE "${CONF_FILE}")
if(NOT DEFINED CONF_FILE OR "${CONF_FILE}" STREQUAL "")
    set(KERFUR_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/prj.conf")
endif()

if("${BOARD}" STREQUAL "nice_nano" AND
   NOT "${KERFUR_CONF_FILE}" MATCHES "(^|[ ;])[^ ;]*prj_release\\.conf($|[ ;])")
    list(APPEND EXTRA_DTC_OVERLAY_FILE
         ${CMAKE_CURRENT_SOURCE_DIR}/boards/nice_nano_usb.overlay)
endif()

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND SHIELD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
find_package(Python3 REQUIRED COMPONENTS Interpreter)

project(kerfur)

target_include_directories(app PRIVATE include)

set(KERFUR_FACE_JSON ${CMAKE_CURRENT_SOURCE_DIR}/assets/face/kerfur_faces.json)
set(KERFUR_FACE_CODEGEN ${CMAKE_CURRENT_SOURCE_DIR}/tools/face_codegen.py)
set(KERFUR_FACE_GEN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ui/generated)
set(KERFUR_FACE_GEN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/generated)
set(KERFUR_FACE_GEN_HEADERS
    ${KERFUR_FACE_GEN_INCLUDE_DIR}/kerfur_face_assets.h
    ${KERFUR_FACE_GEN_INCLUDE_DIR}/kerfur_face_recipes.h
)
set(KERFUR_FACE_GEN_SOURCES
    ${KERFUR_FACE_GEN_SRC_DIR}/kerfur_face_assets.c
    ${KERFUR_FACE_GEN_SRC_DIR}/kerfur_face_recipes.c
)

file(GLOB_RECURSE KERFUR_FACE_ASSET_INPUTS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    assets/face/*.svg
    assets/face/*.json
    assets/face/*.bin
)
list(TRANSFORM KERFUR_FACE_ASSET_INPUTS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

add_custom_command(
    OUTPUT ${KERFUR_FACE_GEN_HEADERS} ${KERFUR_FACE_GEN_SOURCES}
    COMMAND ${Python3_EXECUTABLE} ${KERFUR_FACE_CODEGEN}
            --input ${KERFUR_FACE_JSON}
            --out-include ${KERFUR_FACE_GEN_INCLUDE_DIR}
            --out-src ${KERFUR_FACE_GEN_SRC_DIR}
    DEPENDS ${KERFUR_FACE_CODEGEN} ${KERFUR_FACE_ASSET_INPUTS}
    COMMENT "Generating Kerfur face assets and recipes"
    VERBATIM
)

target_sources(app PRIVATE
    src/main.c
    src/app/app.c
    src/core/event_bus.c
    src/behavior/behavior_engine.c
    src/behavior/micro_reaction.c
    src/display/display_policy.c
    src/ui/ui_renderer.c
    src/ui/face_runtime.c
    ${KERFUR_FACE_GEN_SOURCES}
    src/drivers/motion_sensor.c
    src/drivers/motion_classifier.c
    src/drivers/in_hand_detector.c
    src/drivers/mock_inputs.c
    src/drivers/touch_input.c
    src/drivers/touch_gestures.c
    src/power/power_manager.c
    src/ble/ble_manager.c
)

if(CONFIG_KERFUR_EMOTION_MEMORY)
    target_sources(app PRIVATE src/behavior/emotion_memory.c)
endif()

if(CONFIG_KERFUR_BATTERY)
    target_sources(app PRIVATE src/power/battery_monitor.c)
    if(CONFIG_KERFUR_CHARGER_BQ2563X)
        target_sources(app PRIVATE src/power/bq25630.c)
    endif()
endif()

if(CONFIG_KERFUR_TOUCH_IQS7222A)
    target_sources(app PRIVATE
        src/drivers/iqs7222a.c
        src/drivers/kerfur_iqs7222a_config.c
    )
endif()

if(CONFIG_KERFUR_ENABLE_NEARBY)
    target_sources(app PRIVATE
        src/nearby/kerfur_nearby.c
        src/nearby/encounter_log.c
    )
endif()

if(CONFIG_KERFUR_ENABLE_SHELL_CMDS)
    target_sources(app PRIVATE
        src/ble/ble_shell.c
    )
endif()
