cmake_minimum_required(VERSION 3.20)
project(stargazer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(/W4 /utf-8 /permissive- /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(MODEL_SOURCES
  src/model/rowformat.cpp
  src/model/paths.cpp
  src/model/search.cpp
  src/model/store.cpp
  src/model/dirlist.cpp
  src/model/todo_kind.cpp
  src/model/bgra.cpp
)

add_executable(test_model tests/test_model.cpp ${MODEL_SOURCES})
target_include_directories(test_model PRIVATE src)

add_executable(test_io tests/test_io.cpp
  src/text_io.cpp src/persist.cpp src/model/paths.cpp src/model/rowformat.cpp
  src/dragdrop.cpp src/png.cpp src/clipboard.cpp)
target_include_directories(test_io PRIVATE src)
target_link_libraries(test_io PRIVATE shell32 ole32 advapi32 windowscodecs)

add_executable(test_layout tests/test_layout.cpp src/views/grid_layout.cpp)
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
  src/png.cpp
  src/views/grid.cpp
  src/views/grid_layout.cpp
  src/views/box.cpp
  src/views/browse.cpp
  src/dragdrop.cpp
)

add_executable(stargazer WIN32 ${SG_SOURCES} ${MODEL_SOURCES})
target_include_directories(stargazer PRIVATE src)
target_compile_options(stargazer PRIVATE /O2)
target_link_libraries(stargazer PRIVATE
  d2d1 dwrite shell32 ole32 shlwapi comctl32 dwmapi user32 gdi32 windowscodecs)
