# cleanroom/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(libinterlnk VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(INTERLNK_BUILD_TESTS "Build C99 libinterlnk unit and loopback tests" ON)
option(INTERLNK_BUILD_DESKTOP "Build native desktop CLI tools" ON)

add_library(interlnk_static STATIC
    src/interlnk.c
    src/crc16.c
    src/protocol.c
    src/storage_mem.c
)

target_include_directories(interlnk_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(interlnk_static PRIVATE /W4 /WX)
    target_compile_definitions(interlnk_static PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(interlnk_static PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

if(INTERLNK_BUILD_DESKTOP)
    add_executable(ilnk-server desktop/main_server.c)
    target_link_libraries(ilnk-server PRIVATE interlnk_static)

    add_executable(ilnk-client desktop/main_client.c)
    target_link_libraries(ilnk-client PRIVATE interlnk_static)

    add_executable(ilnk-vector-check desktop/vector_check.c)
    target_link_libraries(ilnk-vector-check PRIVATE interlnk_static)

    foreach(tool_target ilnk-server ilnk-client ilnk-vector-check)
        if(MSVC)
            target_compile_options(${tool_target} PRIVATE /W4 /WX)
            target_compile_definitions(${tool_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(${tool_target} PRIVATE -Wall -Wextra -Werror -pedantic)
        endif()
    endforeach()
endif()

if(INTERLNK_BUILD_TESTS)
    enable_testing()
    add_executable(test_libinterlnk tests/test_libinterlnk.c)
    target_link_libraries(test_libinterlnk PRIVATE interlnk_static)
    if(MSVC)
        target_compile_options(test_libinterlnk PRIVATE /W4 /WX)
        target_compile_definitions(test_libinterlnk PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(test_libinterlnk PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME libinterlnk_unit_loopback COMMAND test_libinterlnk)
endif()
