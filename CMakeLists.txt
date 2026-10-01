cmake_minimum_required(VERSION 3.16)
project(options_pricing_greeks_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

find_package(Threads REQUIRED)

include_directories(include)

add_executable(test_suite tests/test_main.cpp)
target_link_libraries(test_suite PRIVATE Threads::Threads)

add_executable(bench_bs_grid apps/bench_bs_grid.cpp)
target_link_libraries(bench_bs_grid PRIVATE Threads::Threads)

add_executable(bench_variance_reduction apps/bench_variance_reduction.cpp)
target_link_libraries(bench_variance_reduction PRIVATE Threads::Threads)

add_executable(bench_greeks apps/bench_greeks.cpp)
target_link_libraries(bench_greeks PRIVATE Threads::Threads)

add_executable(bench_barrier apps/bench_barrier.cpp)
target_link_libraries(bench_barrier PRIVATE Threads::Threads)

add_executable(svi_fit apps/svi_fit.cpp)

add_executable(screener apps/screener.cpp)

add_executable(american_cross_check apps/american_cross_check.cpp)

add_executable(convergence_check apps/convergence_check.cpp)
target_link_libraries(convergence_check PRIVATE Threads::Threads)

add_executable(vol_store_invert apps/vol_store_invert.cpp)
target_link_libraries(vol_store_invert PRIVATE Threads::Threads)

add_executable(vol_store_generate_prices apps/vol_store_generate_prices.cpp)
target_link_libraries(vol_store_generate_prices PRIVATE Threads::Threads)

enable_testing()
add_test(NAME hand_rolled_suite COMMAND test_suite)
