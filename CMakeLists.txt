cmake_minimum_required(VERSION 3.20)
project(stargazer CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /utf-8 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

set(MODEL_SOURCES
  src/model/rowformat.cpp
  src/model/paths.cpp
  src/model/search.cpp
  src/model/store.cpp
  src/model/bgra.cpp
)

add_executable(test_model tests/test_model.cpp ${MODEL_SOURCES})
target_include_directories(test_model PRIVATE src)

add_executable(test_io tests/test_io.cpp
  src/text_io.cpp src/persist.cpp src/model/paths.cpp src/model/rowformat.cpp
  src/dragdrop.cpp src/clipboard.cpp)
target_include_directories(test_io PRIVATE src)
target_link_libraries(test_io PRIVATE shell32 ole32 advapi32)

add_executable(test_layout tests/test_layout.cpp
  src/views/grid_layout.cpp src/views/shell_layout.cpp)
target_include_directories(test_layout PRIVATE src)

set(SG_SOURCES
  src/main.cpp
  src/render.cpp
  src/app.cpp
  src/text_io.cpp
  src/persist.cpp
  src/icons.cpp
  src/fs_work.cpp
  src/edit.cpp
  src/clipboard.cpp
  src/views/grid.cpp
  src/views/grid_layout.cpp
  src/views/shell_layout.cpp
  src/views/box.cpp
  src/dragdrop.cpp
)

add_executable(stargazer WIN32 ${SG_SOURCES} ${MODEL_SOURCES} resources/stargazer.rc)
target_include_directories(stargazer PRIVATE src resources)
if(MSVC)
  target_compile_options(stargazer PRIVATE /O2)
  # 应用清单写在 resources/stargazer.rc 里；关掉链接器自己那份，免得两份打架。
  target_link_options(stargazer PRIVATE "/MANIFEST:NO")
else()
  target_compile_options(stargazer PRIVATE -O2)
  # MinGW 的 -mwindows 默认找 WinMain；入口是 wWinMain。
  target_link_options(stargazer PRIVATE -municode)
endif()
target_link_libraries(stargazer PRIVATE
  d2d1 dwrite shell32 ole32 shlwapi comctl32 dwmapi user32 gdi32 uxtheme)
