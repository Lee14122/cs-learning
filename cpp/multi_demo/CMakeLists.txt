cmake_minimum_required(VERSION 3.10)
project(./cs-learning/cpp/multi_demo)

set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(core_lib STATIC src/math.cpp src/thread_pool.cpp)
target_include_directories(core_lib PUBLIC include)
target_include_directories(core_lib PRIVATE src)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE core_lib)
