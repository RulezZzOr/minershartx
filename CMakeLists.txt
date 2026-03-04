cmake_minimum_required(VERSION 3.24)
project(minershartx LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES native)
endif()

add_executable(minershartx
  src/main.cpp
  src/pool_miner.cpp
  src/sha256_cpu.cpp
  src/sha256_benchmark.cu
)

target_include_directories(minershartx PRIVATE
  include
  third_party
)

if(MSVC)
  target_compile_options(minershartx PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/O2>
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  )
else()
  target_compile_options(minershartx PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-O3>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
  )
endif()

target_compile_options(minershartx PRIVATE
  $<$<COMPILE_LANGUAGE:CUDA>:--use_fast_math>
  $<$<COMPILE_LANGUAGE:CUDA>:-O3>
  $<$<COMPILE_LANGUAGE:CUDA>:--ptxas-options=-O3>
)

if(WIN32)
  target_link_libraries(minershartx PRIVATE ws2_32)
endif()

set_target_properties(minershartx PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
  CUDA_RUNTIME_LIBRARY Shared
)
