cmake_minimum_required(VERSION 3.16)
project(mattbar CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-cursor)
pkg_check_modules(CAIRO   REQUIRED cairo)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
pkg_check_modules(XKB     REQUIRED xkbcommon)
# SVG tray icons need librsvg AT RUNTIME only (dlopen on first SVG icon);
# there is no build-time dependency and no standing RAM cost without SVGs.

# --- generate Wayland protocol glue ----------------------------------------
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${GEN})

function(wayland_protocol XML BASENAME)
  add_custom_command(
    OUTPUT ${GEN}/${BASENAME}-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${XML}
            ${GEN}/${BASENAME}-client-protocol.h
    # 'namespace' is a valid C identifier but a C++ keyword; rename it.
    # Both offending occurrences are directly followed by ')'.
    COMMAND sed -i "s/namespace)/name_space)/g"
            ${GEN}/${BASENAME}-client-protocol.h
    VERBATIM
    DEPENDS ${XML})
  add_custom_command(
    OUTPUT ${GEN}/${BASENAME}-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${XML}
            ${GEN}/${BASENAME}-protocol.c
    DEPENDS ${XML})
  set(PROTO_SRC ${PROTO_SRC}
      ${GEN}/${BASENAME}-protocol.c
      ${GEN}/${BASENAME}-client-protocol.h PARENT_SCOPE)
endfunction()

wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
                 wlr-layer-shell-unstable-v1)
wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
                 xdg-shell)
wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml
  cursor-shape-v1)
wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/tablet/tablet-v2.xml
  tablet-v2) # cursor-shape references the tablet-tool interface
wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/idle-inhibit-unstable-v1.xml
                 idle-inhibit-unstable-v1)
wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/viewporter.xml
                 viewporter)
wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/fractional-scale-v1.xml
                 fractional-scale-v1)
wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml
                 ext-session-lock-v1)
wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml
                 ext-idle-notify-v1)

# --- executable -------------------------------------------------------------
add_executable(mattbar
  src/main.cpp
  src/bar.cpp
  src/config.cpp
  src/omarchy_theme.cpp
  src/notify.cpp
  src/sensors.cpp
  src/settings.cpp
  src/modules.cpp
  src/panels.cpp
  src/audio.cpp
  src/ctl.cpp
  src/tray.cpp
  src/shell.cpp
  src/menu.cpp
  src/wallpaper.cpp
  src/lock.cpp
  src/polkit.cpp
  src/idle.cpp
  src/nightlight.cpp
  src/weather.cpp
  src/shell_agents.cpp
  src/shell_more.cpp
  src/mods_desk.cpp
  src/shell_panels.cpp
  src/shell_network.cpp
  src/shell_overlays.cpp
  src/qs_plugins.cpp
  ${PROTO_SRC})

target_include_directories(mattbar PRIVATE
  ${GEN}
  ${WAYLAND_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS}
  ${SYSTEMD_INCLUDE_DIRS}
  ${XKB_INCLUDE_DIRS})

find_library(PAM_LIBRARY pam REQUIRED)
target_link_libraries(mattbar PRIVATE
  ${WAYLAND_LIBRARIES}
  ${CAIRO_LIBRARIES}
  ${SYSTEMD_LIBRARIES}
  ${XKB_LIBRARIES}
  ${PAM_LIBRARY}
  ${CMAKE_DL_LIBS})

install(TARGETS mattbar RUNTIME DESTINATION bin)
# mattbarctl is the same binary dispatching on argv[0]
install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink \
  mattbar \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/mattbarctl)")
