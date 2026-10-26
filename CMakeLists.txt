cmake_minimum_required(VERSION 3.20)
project(meridian LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(meridian_core INTERFACE)
target_include_directories(meridian_core INTERFACE ${CMAKE_SOURCE_DIR}/include)

add_executable(timer_overhead bench/timer_overhead.cpp)
target_link_libraries(timer_overhead PRIVATE meridian_core)

enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE meridian_core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(parse_bench bench/parse_bench.cpp)
target_link_libraries(parse_bench PRIVATE meridian_core)

add_executable(test_endianness tests/test_endianness.cpp)
target_link_libraries(test_endianness PRIVATE meridian_core)
add_test(NAME endianness COMMAND test_endianness)

add_executable(exchange_sim src/exchange_sim.cpp)
target_link_libraries(exchange_sim PRIVATE meridian_core)

add_executable(order_client src/order_client.cpp)
target_link_libraries(order_client PRIVATE meridian_core)
