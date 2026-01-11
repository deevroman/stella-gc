cmake_minimum_required(VERSION 3.29)
project(stella C)

set(CMAKE_C_STANDARD 11)

if(NOT DEFINED MAX_ALLOC_SIZE)
    set(MAX_ALLOC_SIZE 33554432) # 1024 * 1024 * 32
endif()

if(DEFINED OFF_WRITE_BARRIER)
    add_compile_options(-DOFF_WRITE_BARRIER)
endif()

if(DEFINED EPSILON_GC)
    add_compile_options(-DEPSILON_GC)                                                  # просто malloc() без сборки мусора
endif()

if(DEFINED GC_DEBUG)
    add_compile_options(-DGC_DEBUG)                                                    # отладочные логи и проверка целостности объектов
endif()

if(DEFINED STELLA_LOGS)
    add_compile_options(-DSTELLA_DEBUG -DSTELLA_GC_STATS -DSTELLA_RUNTIME_STATS)       # полные логи Stella
endif()

if(DEFINED GC_STATES)
    add_compile_options(-DGC_STATES)                                                  # вывод print_gc_state
endif()

if(DEFINED SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)                    # сборка с санитайзерами
    add_link_options(-fsanitize=address)
endif()

add_compile_options(-DMAX_ALLOC_SIZE=\(size_t\)${MAX_ALLOC_SIZE})
add_compile_options(-DSTELLA_GC_STATS -DSTELLA_RUNTIME_STATS)                          # логи Stella только со статистикой

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g")

add_executable(stella
        main.c
        stella/gc.h
        stella/runtime.h
        stella/runtime.c
        stella/gc.c
)
