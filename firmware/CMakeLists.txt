# EVREST L2 EVSE firmware.
#
# Expects the CubeMX-generated Drivers/ and Middlewares/ trees alongside this
# file. Only App/ and Core/ are listed explicitly; the HAL and middleware are
# globbed, because CubeMX rewrites their file lists whenever the .ioc changes
# and a hand-maintained list would silently go stale.

cmake_minimum_required(VERSION 3.22)

project(evrest-evse C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(TARGET_NAME evrest-evse)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

file(GLOB APP_SOURCES     ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/*.c)
file(GLOB CORE_SOURCES    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c)
file(GLOB HAL_SOURCES     ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H5xx_HAL_Driver/Src/*.c)
file(GLOB FREERTOS_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c)

# Template files are generated but not meant to be compiled.
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

add_executable(${TARGET_NAME}
    ${APP_SOURCES}
    ${CORE_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Startup/startup_stm32h573xx.s
)

target_include_directories(${TARGET_NAME} PRIVATE
    App/Inc
    Core/Inc
    Drivers/STM32H5xx_HAL_Driver/Inc
    Drivers/CMSIS/Device/ST/STM32H5xx/Include
    Drivers/CMSIS/Include
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32H573xx
    # Set to 1 once LwIP and mbedTLS are in the tree; net_link.c falls back to
    # the Wi-Fi-only path when this is 0, so the firmware still builds and runs
    # on a board with no Ethernet populated.
    EVSE_USE_LWIP=0
)

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------

set(MCU_FLAGS -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard)

target_compile_options(${TARGET_NAME} PRIVATE
    ${MCU_FLAGS}
    -Wall -Wextra
    # The application layer is held to a stricter standard than the generated
    # code, which does not survive these.
    $<$<COMPILE_LANGUAGE:C>:-Wshadow>
    -ffunction-sections -fdata-sections
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O2 -g>
)

target_link_options(${TARGET_NAME} PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/STM32H573VITX_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=${TARGET_NAME}.map,--cref
    -Wl,--print-memory-usage
    --specs=nano.specs
    -u _printf_float          # meter values are formatted with %f
)

target_link_libraries(${TARGET_NAME} PRIVATE c m nosys)

# ---------------------------------------------------------------------------
# Artefacts
# ---------------------------------------------------------------------------

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET_NAME}> ${TARGET_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET_NAME}> ${TARGET_NAME}.hex
    COMMENT "Generating .bin and .hex"
)
