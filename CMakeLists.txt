cmake_minimum_required(VERSION 3.20)
project(live_sampler VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)

add_library(live_sampler_core
  src/sampler_engine.cpp
  src/jack_client.cpp
)
target_include_directories(live_sampler_core PUBLIC src)
target_link_libraries(live_sampler_core PUBLIC PkgConfig::JACK PkgConfig::RUBBERBAND)
target_compile_options(live_sampler_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(live_sampler_core PRIVATE LIVE_SAMPLER_RUBBERBAND_VERSION=\"${RUBBERBAND_VERSION}\")

add_executable(live-sampler src/main.cpp)
target_link_libraries(live-sampler PRIVATE live_sampler_core)
target_compile_options(live-sampler PRIVATE -Wall -Wextra -Wpedantic)

add_executable(live-sampler-tests tests/test_core.cpp)
target_link_libraries(live-sampler-tests PRIVATE live_sampler_core)
target_compile_options(live-sampler-tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(live-sampler-lifetime-saturation-test tests/test_lifetime_saturation.cpp)
target_link_libraries(live-sampler-lifetime-saturation-test PRIVATE live_sampler_core)
target_compile_options(live-sampler-lifetime-saturation-test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pilot_jack_driver tools/pilot_jack_driver.cpp)
target_link_libraries(pilot_jack_driver PRIVATE PkgConfig::JACK)
target_compile_options(pilot_jack_driver PRIVATE -Wall -Wextra -Wpedantic)

add_executable(rubberband_rt_probe tools/rubberband_rt_probe.cpp)
target_link_libraries(rubberband_rt_probe PRIVATE PkgConfig::RUBBERBAND)
target_compile_options(rubberband_rt_probe PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
add_test(NAME core COMMAND live-sampler-tests)
add_test(NAME lifetime_saturation COMMAND live-sampler-lifetime-saturation-test)
