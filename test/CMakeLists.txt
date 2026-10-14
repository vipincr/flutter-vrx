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
