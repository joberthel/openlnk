# cleanroom/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(libopenlnk VERSION 0.3.0 LANGUAGES C) # x-release-please-version

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(OPENLNK_BUILD_TESTS "Build C99 libopenlnk unit and loopback tests" ON)
option(OPENLNK_BUILD_DESKTOP "Build native desktop CLI tools" ON)

add_library(openlnk_static STATIC
    src/openlnk.c
    src/crc16.c
    src/protocol.c
    src/storage_mem.c
    src/server.c
    src/client.c
)
set_target_properties(openlnk_static PROPERTIES OUTPUT_NAME openlnk)
add_library(openlnk::openlnk ALIAS openlnk_static)

target_include_directories(openlnk_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(openlnk_static PRIVATE /W4 /WX)
    target_compile_definitions(openlnk_static PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(openlnk_static PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

if(OPENLNK_BUILD_DESKTOP)
    add_executable(opensvr desktop/main_server.c desktop/vfat_dir.c)
    target_link_libraries(opensvr PRIVATE openlnk_static)

    add_executable(openlnk desktop/main_client.c)
    target_link_libraries(openlnk PRIVATE openlnk_static)

    add_executable(openlnk-vector-check desktop/vector_check.c)
    target_link_libraries(openlnk-vector-check PRIVATE openlnk_static)

    foreach(tool_target opensvr openlnk openlnk-vector-check)
        if(MSVC)
            target_compile_options(${tool_target} PRIVATE /W4 /WX)
            target_compile_definitions(${tool_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(${tool_target} PRIVATE -Wall -Wextra -Werror -pedantic)
        endif()
    endforeach()
endif()

if(OPENLNK_BUILD_TESTS)
    enable_testing()
    add_executable(test_libopenlnk tests/test_libopenlnk.c)
    target_link_libraries(test_libopenlnk PRIVATE openlnk_static)
    if(MSVC)
        target_compile_options(test_libopenlnk PRIVATE /W4 /WX)
        target_compile_definitions(test_libopenlnk PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(test_libopenlnk PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME libopenlnk_unit_loopback COMMAND test_libopenlnk)

    add_executable(test_dos_hal
        tests/test_dos_hal.c
        dos/dos_hal_uart.c
        dos/dos_hal_disk.c
        dos/dos_hal_io.c
        dos/openlnk_sys_stub.c
    )
    target_include_directories(test_dos_hal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dos)
    target_link_libraries(test_dos_hal PRIVATE openlnk_static)
    if(MSVC)
        target_compile_options(test_dos_hal PRIVATE /W4 /WX)
        target_compile_definitions(test_dos_hal PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(test_dos_hal PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME libopenlnk_dos_hal_16bit COMMAND test_dos_hal)
endif()
