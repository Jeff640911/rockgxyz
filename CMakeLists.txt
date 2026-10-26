cmake_minimum_required(VERSION 3.16)
project(mbox2-driver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

option(MBOX_BUILD_HAL "Build the Core Audio AudioServerPlugIn (macOS only)" ${APPLE})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

# Pure code shared by the daemon and the plug-in: no libusb dependency, so
# the plug-in can link it without pulling USB code into coreaudiod.
add_library(mbox_shared STATIC
  core/src/models.c
  core/src/pcm.c
  core/src/midi.c
  core/src/ring.c
  core/src/shm.c
  core/src/clock.c
  core/src/hosttime.c)
target_include_directories(mbox_shared PUBLIC core/include)
set_target_properties(mbox_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT APPLE)
  target_link_libraries(mbox_shared PUBLIC m)
endif()

add_library(mbox_usb STATIC core/src/device.c core/src/stream.c)
target_link_libraries(mbox_usb PUBLIC mbox_shared PkgConfig::LIBUSB Threads::Threads)

add_executable(mboxctl tools/mboxctl.c)
target_link_libraries(mboxctl PRIVATE mbox_usb)

add_executable(mboxd daemon/mboxd.c daemon/transport.c)
target_link_libraries(mboxd PRIVATE mbox_usb)
if(APPLE)
  target_link_libraries(mboxd PRIVATE "-framework CoreFoundation")
else()
  target_link_libraries(mboxd PRIVATE rt)
endif()

if(APPLE)
  add_executable(mboxmidi daemon/mboxmidi.c hal/transport_client.c)
  target_link_libraries(mboxmidi PRIVATE mbox_shared
    "-framework CoreFoundation" "-framework CoreMIDI")
endif()

if(MBOX_BUILD_HAL)
  add_library(MboxHAL MODULE hal/MboxHAL.c hal/transport_client.c)
  target_link_libraries(MboxHAL PRIVATE mbox_shared
    "-framework CoreAudio" "-framework CoreFoundation")
  set_target_properties(MboxHAL PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    OUTPUT_NAME Mbox2
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/hal/Info.plist)
endif()

enable_testing()
foreach(t pcm midi ring pacer clock shm models)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE mbox_usb)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
