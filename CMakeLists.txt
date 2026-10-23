cmake_minimum_required(VERSION 3.20)
project(xoi VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
include(FetchContent)

find_package(CURL REQUIRED)

option(XOI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(XOI_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

function(xoi_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(XOI_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
    )
    if(XOI_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(XOI_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
      )
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

add_library(xoi
  src/agent.cpp
  src/client.cpp
  src/curl_transport.cpp
)
add_library(xoi::xoi ALIAS xoi)
target_compile_features(xoi PUBLIC cxx_std_20)
target_include_directories(xoi
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(xoi
  PRIVATE
    CURL::libcurl
)
xoi_enable_warnings(xoi)

if(BUILD_TESTING)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googletest)
  add_executable(xoi_tests tests/client_test.cpp)
  target_include_directories(xoi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(xoi_tests PRIVATE xoi::xoi GTest::gtest_main)
  xoi_enable_warnings(xoi_tests)
  include(GoogleTest)
  gtest_discover_tests(xoi_tests)
endif()

add_executable(xoi_chat examples/chat.cpp)
target_link_libraries(xoi_chat PRIVATE xoi::xoi)
xoi_enable_warnings(xoi_chat)

install(TARGETS xoi EXPORT xoiTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY
  ${nlohmann_json_SOURCE_DIR}/include/nlohmann
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT xoiTargets
  FILE xoiTargets.cmake
  NAMESPACE xoi::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xoi
)
configure_package_config_file(
  cmake/xoiConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/xoiConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xoi
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/xoiConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/xoiConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/xoiConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xoi
)
