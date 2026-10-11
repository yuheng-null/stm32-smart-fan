cmake_minimum_required(VERSION 3.22)
project(smart_fan_tests C)

#
# 宿主机单元测试工程 —— 与固件工程（顶层 CMakeLists.txt）完全隔离。
#
# 顶层那一个是**交叉编译**工程，产出 arm 的 elf；本工程用**宿主机**编译器把
# module/ 下的纯计算代码编译成原生可执行文件，直接在本机跑，不接板子、不需要
# ST-Link。这就是 module/ 层「不许出现 HAL/寄存器/CMSIS」那条规矩换来的好处：
# 业务逻辑可以脱离硬件做回归。
#
# 用法（WORKSPACE 根执行）：
#   cmake -S tests -B tests/build -G Ninja
#   cmake --build tests/build
#   ctest --test-dir tests/build --output-on-failure
# 或直接用 tests/CMakePresets.json 的 host 预设。
#

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()

set(SMART_FAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

#
# 被测的 module 源码。
#
# ⚠️ 新增 module 时在这里补上对应的 .c，否则新写的用例会链接失败
#    （症状是 undefined reference，而不是编译错误——因为头文件在 include 路径里）。
#
set(SMART_FAN_MODULE_SOURCES
    "${SMART_FAN_ROOT}/module/temperature/temperature.c"
    "${SMART_FAN_ROOT}/module/fan_control/fan_control.c"
    "${SMART_FAN_ROOT}/module/fan_curve/fan_curve.c"
    "${SMART_FAN_ROOT}/module/pi_control/pi_control.c"
    "${SMART_FAN_ROOT}/module/thermal_control/thermal_control.c"
    "${SMART_FAN_ROOT}/module/fan_fsm/fan_fsm.c"
    "${SMART_FAN_ROOT}/module/console/console.c"
    "${SMART_FAN_ROOT}/module/telemetry/telemetry.c"
    "${SMART_FAN_ROOT}/module/display_view/display_view.c"
)

add_library(smart_fan_module STATIC ${SMART_FAN_MODULE_SOURCES})

# 把工程根放进 include 路径，于是源码里 `#include "module/xxx.h"` 原样可用，
# 与固件工程里的写法完全一致。
target_include_directories(smart_fan_module PUBLIC "${SMART_FAN_ROOT}")

# module/ 是本项目自己的代码，按固件那边的标准要求它零警告。
target_compile_options(smart_fan_module PRIVATE -Wall -Wextra -Werror)

# logf / lroundf 在 libm 里。MinGW 上 libm 可能不存在（数学函数在 msvcrt 里），
# 所以探测到再链，而不是无脑写 -lm。
find_library(SMART_FAN_MATH_LIBRARY m)
if(SMART_FAN_MATH_LIBRARY)
    target_link_libraries(smart_fan_module PUBLIC "${SMART_FAN_MATH_LIBRARY}")
endif()

#
# Unity 测试框架（vendored，归属见仓库根 NOTICE）。
# 它是第三方代码，不套本工程的 -Werror —— 上游的告警不该拦我们的构建。
#
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)

#
# 每个 tests/test_*.c 编成一个独立可执行文件，并注册进 CTest。
# 独立可执行文件而不是一个大 main：单个用例崩溃只影响自己，
# ctest 也能精确报出是哪一个挂了。
#
function(smart_fan_add_test name)
    add_executable(${name} "${name}.c")
    target_link_libraries(${name} PRIVATE smart_fan_module unity)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

smart_fan_add_test(test_temperature)
smart_fan_add_test(test_fan_control)
smart_fan_add_test(test_fan_curve)
smart_fan_add_test(test_pi_control)
smart_fan_add_test(test_thermal_control)
smart_fan_add_test(test_fan_fsm)
smart_fan_add_test(test_console)
smart_fan_add_test(test_telemetry)
smart_fan_add_test(test_display_view)
