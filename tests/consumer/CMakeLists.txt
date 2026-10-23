cmake_minimum_required(VERSION 3.20)
project(xoi_consumer_smoke LANGUAGES CXX)

find_package(xoi CONFIG REQUIRED)

add_executable(xoi_consumer_smoke main.cpp)
target_compile_features(xoi_consumer_smoke PRIVATE cxx_std_20)
target_link_libraries(xoi_consumer_smoke PRIVATE xoi::xoi)
