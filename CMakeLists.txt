cmake_minimum_required(VERSION 3.20)

project(yggdrasil LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(ENGINE_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(ENGINE_BUILD_TESTS "Build unit tests" ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# Project options
add_library(yggdrasil_project_options INTERFACE)

engine_set_project_warnings(yggdrasil_project_options)
engine_enable_sanitizers(yggdrasil_project_options)

# Library
add_library(yggdrasil
    src/database.cc
    src/storage/disk/disk_manager.cc
    src/storage/page/slotted_page.cc
    src/buffer/buffer_pool_manager.cc
    src/buffer/replacer/clock_replacer.cc
    src/buffer/replacer/lru_replacer.cc
)

target_include_directories(yggdrasil
    PUBLIC
        include
)

target_link_libraries(yggdrasil
    PRIVATE
        yggdrasil_project_options
)

add_executable(page_dump tools/page_dump.cc )
target_link_libraries(page_dump PRIVATE yggdrasil yggdrasil_project_options)

add_executable(create_db
    tools/create_db.cc
)

target_link_libraries(create_db
    PRIVATE
        yggdrasil
        yggdrasil_project_options
)

# Tests
if(ENGINE_BUILD_TESTS)
    enable_testing()

    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    add_executable(yggdrasil_unit_tests
        tests/unit/database_test.cc
        tests/unit/storage/disk/disk_manager_test.cc
        tests/unit/storage/page/slotted_page_test.cc
        tests/unit/buffer/buffer_pool_manager_test.cc
        tests/unit/buffer/replacer/clock_replacer_test.cc
        tests/unit/buffer/replacer/lru_replacer_test.cc
    )

    target_link_libraries(yggdrasil_unit_tests
        PRIVATE
            yggdrasil
            GTest::gtest_main
            yggdrasil_project_options
    )

    include(GoogleTest)
    gtest_discover_tests(yggdrasil_unit_tests)
endif()

# clang-format
find_program(CLANG_FORMAT_EXE NAMES clang-format)

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE FORMAT_FILES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/*.cc
        ${PROJECT_SOURCE_DIR}/include/*.h
        ${PROJECT_SOURCE_DIR}/tests/*.cc
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_FILES}
        COMMENT "Running clang-format"
        VERBATIM
    )
endif()

# clang-tidy
find_program(CLANG_TIDY_EXE 
  NAMES 
    clang-tidy
    clang-tidy-18
    clang-tidy-17
)

if(CLANG_TIDY_EXE)
    file(GLOB_RECURSE TIDY_FILES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/*.cc
        ${PROJECT_SOURCE_DIR}/tests/*.cc
    )

    add_custom_target(lint
        COMMAND
            ${CLANG_TIDY_EXE}
            -quiet
            -fix
            -p=${PROJECT_BINARY_DIR}
            ${TIDY_FILES}
        COMMENT "Running clang-tidy"
        VERBATIM
    )
endif()