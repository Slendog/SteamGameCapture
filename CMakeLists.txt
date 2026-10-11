cmake_minimum_required(VERSION 3.22)

project(steam-game-capture VERSION 0.1.0 LANGUAGES C)

option(SGC_BUILD_PLUGIN "Build the OBS plugin (needs libobs)" ON)
option(SGC_BUILD_PROBE "Build the sgc-probe diagnostic tool" ON)
set(SGC_INSTALL_LAYOUT "user" CACHE STRING
    "'user': <prefix>/steam-game-capture/{bin/64bit,data} (for ~/.config/obs-studio/plugins); 'system': lib/obs-plugins + share/obs/obs-plugins")
set_property(CACHE SGC_INSTALL_LAYOUT PROPERTY STRINGS user system)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-composite xcb-shm xcb-res xcb-dri3 xcb-xfixes)
pkg_check_modules(DRM REQUIRED libdrm) # drm_fourcc.h only
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# Backends shared by the plugin and the probe tool.
add_library(sgc-core STATIC src/sgc-log.c src/sgc-x11.c src/sgc-pipewire.c)
target_include_directories(sgc-core PUBLIC src ${DRM_INCLUDE_DIRS})
target_compile_definitions(sgc-core PRIVATE _GNU_SOURCE)
target_link_libraries(sgc-core PUBLIC PkgConfig::XCB PkgConfig::PIPEWIRE)
set_target_properties(sgc-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SGC_BUILD_PLUGIN)
  find_package(libobs REQUIRED)

  add_library(steam-game-capture MODULE src/plugin-main.c src/steam-game-capture.c)
  target_link_libraries(steam-game-capture PRIVATE OBS::libobs sgc-core)
  set_target_properties(steam-game-capture PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)

  if(SGC_INSTALL_LAYOUT STREQUAL "system")
    include(GNUInstallDirs)
    install(TARGETS steam-game-capture LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
    install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/steam-game-capture)
  else()
    install(TARGETS steam-game-capture LIBRARY DESTINATION steam-game-capture/bin/64bit)
    install(DIRECTORY data/ DESTINATION steam-game-capture/data)
  endif()
endif()

if(SGC_BUILD_PROBE)
  add_executable(sgc-probe tools/sgc-probe.c)
  target_link_libraries(sgc-probe PRIVATE sgc-core m)
endif()
