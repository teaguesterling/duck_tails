cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duck_tails)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)
find_package(libgit2 CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES src/duck_tails_extension.cpp src/git_filesystem.cpp src/git_functions.cpp src/git_log.cpp src/git_path.cpp src/git_utils.cpp src/git_context_manager.cpp src/git_tree.cpp src/git_parents.cpp src/git_branches.cpp src/git_tags.cpp src/git_read.cpp src/git_uri.cpp src/text_diff.cpp src/git_history.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL and libgit2 in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto libgit2::libgit2package)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto libgit2::libgit2package)

# Create proper directory structure for test framework to find the extension
# This ensures tests use the freshly built extension, not cached versions
# Note: We need to determine the platform at build time
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(PLATFORM_DIR "osx_arm64")
    else()
        set(PLATFORM_DIR "osx_amd64")
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(PLATFORM_DIR "linux_arm64")
    else()
        set(PLATFORM_DIR "linux_amd64")
    endif()
elseif(WIN32)
    set(PLATFORM_DIR "windows_amd64")
endif()

set(TEST_EXTENSION_DIR "${CMAKE_BINARY_DIR}/extension/v${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION}.${DUCKDB_PATCH_VERSION}/${PLATFORM_DIR}")
add_custom_command(
    TARGET ${LOADABLE_EXTENSION_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_EXTENSION_DIR}
    COMMAND ${CMAKE_COMMAND} -E create_symlink 
            $<TARGET_FILE:${LOADABLE_EXTENSION_NAME}>
            ${TEST_EXTENSION_DIR}/${TARGET_NAME}.duckdb_extension
    COMMENT "Creating extension directory structure for testing in ${TEST_EXTENSION_DIR}"
)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
