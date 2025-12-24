cmake_minimum_required(VERSION 3.16)
project(rpi_calendar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

add_executable(rpi_calendar
    src/main.cpp
    src/views/ClockView.cpp
    src/views/CalendarView.cpp
    src/services/CalendarSyncService.cpp
    src/db/EventStore.cpp
    src/util/TimeUtil.cpp
)

target_include_directories(rpi_calendar PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${SDL2_IMAGE_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rpi_calendar PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    SQLite::SQLite3
    CURL::libcurl
)

# nlohmann/json is header-only; assume it is available in include path

target_compile_definitions(rpi_calendar PRIVATE
    SDL_MAIN_HANDLED
)
