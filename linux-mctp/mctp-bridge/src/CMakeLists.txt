cmake_minimum_required(VERSION 3.16)
project(mctp-listener LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options
option(BUILD_TESTS "Build unit/integration tests" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer/UBSan (for CI/dev)" OFF)

if(ENABLE_ASAN)
  message(STATUS "Enabling ASAN/UBSAN flags")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Locate dependencies via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNL3 REQUIRED libnl-3.0 libnl-genl-3.0 libnl-route-3.0)

include_directories(${LIBNL3_INCLUDE_DIRS})
link_directories(${LIBNL3_LIBRARY_DIRS})
add_definitions(${LIBNL3_CFLAGS_OTHER})

# Collect sources (explicit listing is preferred long-term)
file(GLOB_RECURSE MCTP_SRCS CONFIGURE_DEPENDS "*.cpp" "*.c")

add_executable(mctp-bridge ${MCTP_SRCS})

target_include_directories(mctp-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mctp-bridge PRIVATE ${LIBNL3_LIBRARIES})

## This project builds a small utility executable (mctp-bridge).
## Install target provided below; packaging (CPack) intentionally omitted.

# Install
install(TARGETS mctp-bridge RUNTIME DESTINATION bin)

# Convenience custom target so developers can run: make install-app
add_custom_target(install-app
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target install
    COMMENT "Build and install mctp-bridge to the system (requires privileges)"
)

# Tests
if(BUILD_TESTS)
  enable_testing()
  # add_subdirectory(tests) # optional
endif()
cmake_minimum_required(VERSION 3.16)
project(mctp-listener LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options
option(BUILD_TESTS "Build unit/integration tests" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer/UBSan (for CI/dev)" OFF)

if(ENABLE_ASAN)
  message(STATUS "Enabling ASAN/UBSAN flags")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

