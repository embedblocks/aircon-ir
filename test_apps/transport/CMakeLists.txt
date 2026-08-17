cmake_minimum_required(VERSION 3.16)

# This test app lives at components/ac_ir/test_apps/transport/. Two
# levels up is components/ac_ir/ itself -- the component under test.
# EXTRA_COMPONENT_DIRS accepts either a directory containing several
# components, or a single component directory directly; this is the
# latter.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac_ir_transport_test)
