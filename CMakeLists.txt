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

add_executable(test_order_id tests/test_order_id.cpp)
target_link_libraries(test_order_id PRIVATE meridian_core)
add_test(NAME order_id COMMAND test_order_id)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE meridian_core)
add_test(NAME order_book COMMAND test_order_book)

add_executable(book_bench bench/book_bench.cpp)
target_link_libraries(book_bench PRIVATE meridian_core)

add_executable(test_price_bitmap tests/test_price_bitmap.cpp)
target_link_libraries(test_price_bitmap PRIVATE meridian_core)
add_test(NAME price_bitmap COMMAND test_price_bitmap)

add_executable(book_sparse_bench bench/book_sparse_bench.cpp)
target_link_libraries(book_sparse_bench PRIVATE meridian_core)

# old linear-scan variants, kept only to measure before/after
add_executable(test_order_book_linear tests/test_order_book.cpp)
target_link_libraries(test_order_book_linear PRIVATE meridian_core)
target_compile_definitions(test_order_book_linear PRIVATE MERIDIAN_BOOK_LINEAR_SCAN)
add_test(NAME order_book_linear COMMAND test_order_book_linear)

add_executable(book_bench_linear bench/book_bench.cpp)
target_link_libraries(book_bench_linear PRIVATE meridian_core)
target_compile_definitions(book_bench_linear PRIVATE MERIDIAN_BOOK_LINEAR_SCAN)

add_executable(book_sparse_bench_linear bench/book_sparse_bench.cpp)
target_link_libraries(book_sparse_bench_linear PRIVATE meridian_core)
target_compile_definitions(book_sparse_bench_linear PRIVATE MERIDIAN_BOOK_LINEAR_SCAN)

add_executable(empty_tail bench/empty_tail.cpp)
target_link_libraries(empty_tail PRIVATE meridian_core)

add_executable(tlb_probe bench/tlb_probe.cpp)
target_link_libraries(tlb_probe PRIVATE meridian_core)

find_package(Threads REQUIRED)
add_executable(test_spsc tests/test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE meridian_core Threads::Threads)
add_test(NAME spsc COMMAND test_spsc)

add_executable(queue_bench bench/queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE meridian_core Threads::Threads)

add_executable(pipeline_bench bench/pipeline_bench.cpp)
target_link_libraries(pipeline_bench PRIVATE meridian_core Threads::Threads)

add_executable(test_replay tests/test_replay.cpp)
target_link_libraries(test_replay PRIVATE meridian_core)
add_test(NAME replay COMMAND test_replay)

add_executable(replay_bench bench/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE meridian_core)
