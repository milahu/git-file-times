cmake_minimum_required(VERSION 3.20)
project(git-file-times-project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prefer optimized builds
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED libgit2)

add_executable(git-file-times
    src/main.cpp
)

target_include_directories(git-file-times PRIVATE
    ${LIBGIT2_INCLUDE_DIRS}
)

target_link_directories(git-file-times PRIVATE
    ${LIBGIT2_LIBRARY_DIRS}
)

target_link_libraries(git-file-times PRIVATE
    ${LIBGIT2_LIBRARIES}
)

target_compile_options(git-file-times PRIVATE
    ${LIBGIT2_CFLAGS_OTHER}
)

# Extra performance flags (optional but recommended)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(git-file-times PRIVATE
        -O3
        -march=native
        -flto
    )

    target_link_options(git-file-times PRIVATE
        -flto
    )
endif()
