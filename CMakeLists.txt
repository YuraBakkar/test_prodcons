cmake_minimum_required(VERSION 3.12)

project(test_prodcons LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(producer
    src/producer.cpp
)

target_compile_features(producer PRIVATE cxx_std_17)
target_link_libraries(producer PRIVATE Threads::Threads)
set_target_properties(producer PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(producer PRIVATE /W4 /permissive-)
else()
    target_compile_options(producer PRIVATE -Wall -Wextra -Wpedantic)
endif()
