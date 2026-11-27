cmake_minimum_required(VERSION 3.16)

# Register the repo's esp32/ directory directly as a local ESP-IDF component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

# Build only components required by main (omits Wi-Fi, BT, LwIP, mbedTLS, etc.).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openlnk_dos_client)
