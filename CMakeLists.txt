cmake_minimum_required(VERSION 3.12)

project(test_prodcons LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(producer
    src/producer.cpp
)

add_executable(consumer
    src/consumer.cpp
)

target_compile_features(producer PRIVATE cxx_std_17)
target_link_libraries(producer PRIVATE Threads::Threads)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE Threads::Threads)
set_target_properties(producer PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(producer PRIVATE /W4 /permissive-)
    target_compile_options(consumer PRIVATE /W4 /permissive-)
else()
    target_compile_options(producer PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic)
endif()
