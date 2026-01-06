cmake_minimum_required(VERSION 3.20)
project(novaaudio_poc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# --- raylib (window + input only; audio handled by miniaudio in main.c) ---
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG master
)
FetchContent_MakeAvailable(raylib)

add_executable(novaaudio_poc
  src/main.c
  third_party/sonic/sonic.c
)

target_include_directories(novaaudio_poc PRIVATE
  third_party/miniaudio
  third_party/sonic
  third_party/raygui
)

target_link_libraries(novaaudio_poc PRIVATE raylib)

# Miniaudio fix for macOS: avoid runtime framework loading.
# See miniaudio docs re: MA_NO_RUNTIME_LINKING.
target_compile_definitions(novaaudio_poc PRIVATE
  MA_NO_RUNTIME_LINKING
  MA_DEBUG_OUTPUT
)

if(APPLE)
  # Required when MA_NO_RUNTIME_LINKING is enabled.
  target_link_libraries(novaaudio_poc PRIVATE
    "-framework CoreFoundation"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AudioUnit"
  )
endif()

# Linux extras
if(UNIX AND NOT APPLE)
  target_link_libraries(novaaudio_poc PRIVATE m pthread dl)
endif()
