cmake_minimum_required(VERSION 3.31)
project(deck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(FTXUI REQUIRED COMPONENTS screen dom component)
find_package(SQLite3 REQUIRED)

add_library(deck_core
  src/app.cpp
  src/cli.cpp
  src/environment.cpp
  src/event_bus.cpp
  src/finance.cpp
  src/news.cpp
  src/notes.cpp
  src/persistence.cpp
  src/picker.cpp
  src/panes.cpp
  src/process.cpp
  src/split_tree.cpp
  src/types.cpp
  src/workspace.cpp
)

target_include_directories(deck_core PUBLIC include)
target_link_libraries(deck_core
  PUBLIC
    ftxui::screen
    ftxui::dom
    ftxui::component
    SQLite::SQLite3
)

add_executable(deck src/main.cpp)
target_link_libraries(deck PRIVATE deck_core)

if(BUILD_TESTING)
  add_executable(deck_tests
    tests/test_environment.cpp
    tests/test_main.cpp
    tests/test_news.cpp
    tests/test_process.cpp
    tests/test_split_tree.cpp
    tests/test_workspace.cpp
    tests/test_event_bus.cpp
    tests/test_panes.cpp
    tests/test_paper_anchor.cpp
    tests/test_picker.cpp
    tests/test_persistence.cpp
  )
  target_link_libraries(deck_tests PRIVATE deck_core)
  add_test(NAME deck_tests COMMAND deck_tests)
endif()
