cmake_minimum_required(VERSION 3.20)
project(AtariLion VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

add_library(lioncore STATIC
  src/bus.cpp
  src/bootrom.cpp
  src/rom/assets.cpp
  src/rom/intro.cpp
  src/rom/dashboard.cpp
  src/rom/audio.cpp
  src/rom/music.cpp
  src/rom/blobs.cpp
  src/cdrom.cpp
  src/pic.cpp
  src/vi.cpp
  src/pad.cpp
  src/loader.cpp
  src/system.cpp
  src/tuffy/ppc.cpp
  src/tuffy/ppc_fpu.cpp
  src/tuffy/ppc_altivec.cpp
  src/tuffy/ppc_mmu.cpp
  src/tuffy/ppc_sched.cpp
  src/quacker/m88k.cpp
  src/quacker/m88k_sched.cpp
  src/quacker/qctrl.cpp
  src/quacker/qve.cpp
  src/spike/spike.cpp
  src/tyke/tyke.cpp
  src/tyke/firmware.cpp
)
target_include_directories(lioncore PUBLIC src tools)
# Audio baked into the boot ROM, pulled in with .incbin (src/rom/blobs.cpp).
set(LION_ROM_AUDIO
  ${CMAKE_SOURCE_DIR}/Assets/rom/roar.pcm
  ${CMAKE_SOURCE_DIR}/Assets/rom/claws.pcm
  ${CMAKE_SOURCE_DIR}/Assets/rom/theme1.ladp
  ${CMAKE_SOURCE_DIR}/Assets/rom/theme2.ladp)
target_compile_definitions(lioncore PRIVATE LION_ROM_AUDIO_DIR="${CMAKE_SOURCE_DIR}/Assets/rom")
set_source_files_properties(src/rom/blobs.cpp PROPERTIES OBJECT_DEPENDS "${LION_ROM_AUDIO}")
find_package(Threads REQUIRED)
target_link_libraries(lioncore PUBLIC Threads::Threads)

# Encoders used by tests and by the demo builder.
add_library(lionasm INTERFACE)
target_include_directories(lionasm INTERFACE tools)

find_package(SDL2 REQUIRED)
add_executable(lion src/main.cpp)
target_link_libraries(lion PRIVATE lioncore SDL2::SDL2)

# Converts audio for the boot ROM (see tools/convert_assets.sh).
add_executable(lionasset tools/lionasset.cpp)
target_include_directories(lionasset PRIVATE tools)

add_executable(mkdemo tools/mkdemo.cpp)
target_link_libraries(mkdemo PRIVATE lioncore lionasm)

enable_testing()
add_executable(lion_tests
  tests/test_main.cpp
  tests/test_ppc.cpp
  tests/test_m88k.cpp
  tests/test_spike.cpp
  tests/test_system.cpp
  tests/test_mmu.cpp
  tests/test_cdrom.cpp
  tests/test_tyke.cpp
  tests/test_dashboard.cpp
)
target_link_libraries(lion_tests PRIVATE lioncore lionasm)
add_test(NAME lion_tests COMMAND lion_tests)

# Unreal 1 level packager; the PowerPC client is built by unreal/build.sh.
add_executable(unrport unreal/unrport.cpp)
target_link_libraries(unrport PRIVATE lioncore)
