cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME smart_fan)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
# 手写代码一律放在这些与 CubeMX 无关的目录里；Core/、Drivers/ 下的内容
# 由 CubeMX 掌管，除 USER CODE 块外不要动。
#
# 分层见 docs/architecture.md：app -> module -> interface <- driver <- bsp
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    app/app_main.c
    app/app_control.c
    app/app_console.c
    app/app_display.c
    app/app_ir.c

    module/temperature/temperature.c
    module/fan_control/fan_control.c
    module/fan_curve/fan_curve.c
    module/pi_control/pi_control.c
    module/thermal_control/thermal_control.c
    module/fan_fsm/fan_fsm.c
    module/console/console.c
    module/telemetry/telemetry.c
    module/display_view/display_view.c
    module/ir_remote/ir_remote.c

    # 总线驱动（贴 HAL 的那一层）
    driver/bus/i2c/i2c_soft.c

    # 设备驱动（可移植，不含任何 HAL 符号）
    driver/device/ssd1306/ssd1306.c
    driver/device/ssd1306/ssd1306_fonts.c

    # 板级绑定
    bsp/bsp_adc.c
    bsp/bsp_buzzer.c
    bsp/bsp_fan.c
    bsp/bsp_i2c.c
    bsp/bsp_ir.c
    bsp/bsp_key.c
    bsp/bsp_oled.c
    bsp/bsp_relay.c
    bsp/bsp_system.c
    bsp/bsp_time.c
    bsp/bsp_uart.c
)

#
# 手写代码的警告门。
#
# 顶层 CMakeLists 里的 target_compile_options 只作用于**本 target 的源文件**
# （即上面 target_sources 列出的 app/ module/ bsp/ driver/），不会传导到
# cmake/stm32cubemx 那两个库（HAL/CMSIS 是第三方，不按本工程的标准要求它）。
# 这正是我们要的边界：自己的代码严格，厂商代码不动。
#
# 与 tests/CMakeLists.txt 的 -Wall -Wextra -Werror 保持同一套标准：手写代码一律
# 零警告。工具链里已经有 -Wall，这里补 -Wextra。
# 交叉编译侧暂不加 -Werror——arm-none-eabi-gcc 由 CI 镜像提供、版本随镜像浮动，
# 宿主机单测那边则可以按需对齐版本。
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wextra)

# Add include paths
# 工程根入 include 路径，于是源码里可以写 "interface/uart_if.h" 这种
# 从根算起的路径，一眼看出依赖方向。
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
