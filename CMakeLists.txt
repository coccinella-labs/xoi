cmake_minimum_required(VERSION 3.20)
project(xoi VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

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
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(xoi PUBLIC nlohmann_json::nlohmann_json PRIVATE CURL::libcurl)

if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(xoi_tests tests/client_test.cpp)
  target_link_libraries(xoi_tests PRIVATE xoi::xoi GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(xoi_tests)
endif()

add_executable(xoi_chat examples/chat.cpp)
target_link_libraries(xoi_chat PRIVATE xoi::xoi)

install(TARGETS xoi EXPORT xoiTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
