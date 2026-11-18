add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(
    platformchannel_test
    flutterpi_module
    Unity
)

add_test(platformchannel_test platformchannel_test)

add_executable(flutterpi_test
    flutterpi_test.c
)

target_link_libraries(
    flutterpi_test
    flutterpi_module
    Unity
)

add_test(flutterpi_test flutterpi_test)
# vrx camera plugin: the pure wire parts need no flutter-pi and no Linux
add_executable(vrx_camera_wire_test
    vrx_camera_wire_test.c
    ../src/plugins/vrx_camera/wire.c
)

target_include_directories(vrx_camera_wire_test PRIVATE ../src)

target_link_libraries(
    vrx_camera_wire_test
    Unity
)

add_test(vrx_camera_wire_test vrx_camera_wire_test)

# the compositor's VT hold (plan 0028 D2): a fake ioctl, no VT needed
add_executable(vrx_vt_test
    vrx_vt_test.c
    ../src/vrx_vt.c
)

target_include_directories(vrx_vt_test PRIVATE ../src)

target_link_libraries(
    vrx_vt_test
    Unity
)

add_test(vrx_vt_test vrx_vt_test)

# the power-off countdown's wire: numbers and sizes vrxhost encodes
add_executable(vrx_ipc_power_off_wire_test
    vrx_ipc_power_off_wire_test.c
)

target_include_directories(vrx_ipc_power_off_wire_test PRIVATE ../src)

target_link_libraries(
    vrx_ipc_power_off_wire_test
    Unity
)

add_test(vrx_ipc_power_off_wire_test vrx_ipc_power_off_wire_test)

# the light's sequence (plan 0033): the stored brightness, the migration, the fade, retargeting; a fake device and clock
add_executable(vrx_screen_light_test
    vrx_screen_light_test.c
    ../src/vrx_screen_light.c
)

target_include_directories(vrx_screen_light_test PRIVATE ../src)

target_link_libraries(
    vrx_screen_light_test
    Unity
    ${VRX_LIGHT_TARGET}
)

add_test(vrx_screen_light_test vrx_screen_light_test)

# the resume's presentation (plan 0034): the splash in the dark, the wake's wait for the essentials, the dip, the splash file
add_executable(vrx_resume_test
    vrx_resume_test.c
    ../src/vrx_resume.c
)

target_include_directories(vrx_resume_test PRIVATE ../src)

target_link_libraries(
    vrx_resume_test
    Unity
    ${VRX_LIGHT_TARGET}
)

add_test(vrx_resume_test vrx_resume_test)

# vrx-linux, which owns the screen's words (vectors/screen-wire.json) and the UI standard the light's header is generated from
set(VRX_LINUX_DIR "${CMAKE_SOURCE_DIR}/../vrx/yocto/layers/vrx/vrx-linux" CACHE PATH "vrx-linux, the owner of the vectors and of the vendored UI standard")

add_executable(vrx_ipc_screen_wire_test
    vrx_ipc_screen_wire_test.c
)

target_include_directories(vrx_ipc_screen_wire_test PRIVATE ../src)

target_link_libraries(
    vrx_ipc_screen_wire_test
    Unity
)

add_test(NAME vrx_ipc_screen_wire_test COMMAND vrx_ipc_screen_wire_test "${VRX_LINUX_DIR}/vectors/screen-wire.json")

# src/vrx_light_standard.h is generated from vrx-linux's vendored standard: a re-vendor that leaves it stale fails here
# (and in the pre-push hook). A missing standard fails too.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME vrx_light_standard_header
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/ui-standard-light.py"
            "${VRX_LINUX_DIR}/flutter/vrx_kit/standard/vrx-ui-standard.toml" --check)
