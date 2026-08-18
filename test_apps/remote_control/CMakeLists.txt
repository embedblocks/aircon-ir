cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../..        # driver component
)

set(TEST_COMPONENTS "aircon-ir")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aircon-ir-remote-test)