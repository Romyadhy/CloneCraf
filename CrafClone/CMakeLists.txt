cmake_minimum_required(VERSION 3.16)
project(CrafClone CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(CrafClone src/main.cpp)

if(WIN32)
    # Windows: pakai raylib bundled dari libs/
    target_include_directories(CrafClone PRIVATE libs/raylib/include)
    target_link_libraries(CrafClone PRIVATE
        ${CMAKE_SOURCE_DIR}/libs/raylib/lib/libraylib.a
        winmm gdi32 opengl32
    )
else()
    # Linux/macOS: pakai raylib dari sistem (sudo pacman -S raylib)
    find_package(raylib REQUIRED)
    target_link_libraries(CrafClone PRIVATE raylib)
endif()