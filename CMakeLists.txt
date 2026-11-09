cmake_minimum_required(VERSION 3.20)

project(Billiards3D VERSION 1.0.0 DESCRIPTION "3D Billiards" LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

# The shaders and the icon are built into the executable.
file(GLOB SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl ${CMAKE_SOURCE_DIR}/shaders/*.vert
     ${CMAKE_SOURCE_DIR}/shaders/*.frag ${CMAKE_SOURCE_DIR}/shaders/*.comp)
include(cmake/Embed.cmake)
embed_files(${CMAKE_BINARY_DIR}/Embedded.cpp ${SHADERS} ${CMAKE_SOURCE_DIR}/assets/billiards3d.png)

add_executable(billiards3d
    src/Ai.cpp
    src/App.cpp
    src/Camera.cpp
    src/Desktop.cpp
    src/Gl.cpp
    src/Main.cpp
    src/Physics.cpp
    src/Prediction.cpp
    src/Renderer.cpp
    src/Resources.cpp
    src/Rules.cpp
    src/Settings.cpp
    src/Sound.cpp
    src/Strings.cpp
    src/TableModel.cpp
    src/TableView.cpp
    src/Text.cpp
    src/Themes.cpp
    src/UiPainter.cpp
    ${CMAKE_BINARY_DIR}/Embedded.cpp)

target_include_directories(billiards3d PRIVATE src)
target_compile_definitions(billiards3d PRIVATE BILLIARDS3D_VERSION="${PROJECT_VERSION}")
target_compile_features(billiards3d PRIVATE cxx_std_23)
set_target_properties(billiards3d PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(billiards3d PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(billiards3d PRIVATE
    PkgConfig::SDL2 PkgConfig::FREETYPE PkgConfig::FONTCONFIG PkgConfig::PNG PkgConfig::X11 Threads::Threads)

include(GNUInstallDirs)
install(TARGETS billiards3d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/billiards3d.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/billiards3d.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
