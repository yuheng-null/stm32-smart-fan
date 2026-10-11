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

    module/temperature/temperature.c

    # 总线驱动（贴 HAL 的那一层）
    driver/bus/i2c/i2c_soft.c

    # 设备驱动（可移植，不含任何 HAL 符号）
    driver/device/ssd1306/ssd1306.c
    driver/device/ssd1306/ssd1306_fonts.c

    # 板级绑定
    bsp/bsp_adc.c
    bsp/bsp_i2c.c
    bsp/bsp_key.c
    bsp/bsp_oled.c
    bsp/bsp_system.c
    bsp/bsp_uart.c
)

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
