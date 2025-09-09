cmake_minimum_required(VERSION 3.26)
project(shared-mem-ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shmx INTERFACE src/shmx_common.h src/shmx_server.h src/shmx_client.h src/shmx_inspector.h)
target_include_directories(shmx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
foreach (test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE shmx)
    target_compile_options(${test_name} PRIVATE /W4 /WX)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach ()