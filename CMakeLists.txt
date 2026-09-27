cmake_minimum_required(VERSION 3.22)
project(MpcVstMachinedrum LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(MPC_VST_DIR "" CACHE PATH "mpc-vst-plugins checkout (the shared VST2 wrapper); only needed for the plugin .so target")

include(cmake/dsp56300.cmake)

add_library(mdcore STATIC
  engine/HostModel.cpp
  engine/MachineRunner.cpp
  engine/Mixer.cpp
  engine/ParallelVoiceEngine.cpp
  engine/TrackFx.cpp
  engine/VoiceEngine.cpp
  tools/mdfw/Firmware.cpp
)
target_include_directories(mdcore PUBLIC engine tools/mdfw)
target_include_directories(mdcore PRIVATE
  libs/gearmulator-md-mm/source libs/gearmulator-md-mm/source/mc68k)
target_link_libraries(mdcore PUBLIC dsp56kEmu 68kEmu)

# machinedrum_one: the same engine source (vst/engine.cpp) built against the shared wrapper
# (mpc-vst-plugins' vst2_wrap.c). Needs its generated params.h (mpc-vst's gen_vst.py --params-h, run by
# vst/build_so.sh) on its include path first (vst/build/).
if(MPC_VST_DIR)
  add_library(machinedrum_one SHARED vst/engine.cpp "${MPC_VST_DIR}/wrapper/vst2_wrap.c")
  target_include_directories(machinedrum_one PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/build")
  target_link_libraries(machinedrum_one PRIVATE mdcore pthread)
  set_target_properties(machinedrum_one PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  target_link_options(machinedrum_one PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)

  add_executable(md-vst-smoke vst/smoke.cpp vst/engine.cpp)
  target_include_directories(md-vst-smoke PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/build")
  target_link_libraries(md-vst-smoke PRIVATE mdcore pthread)
endif()
