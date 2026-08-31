cmake_minimum_required(VERSION 3.24)
project(occt_fillet_search VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCASCADE 7.8.1 CONFIG REQUIRED)

# This SDK is a Debug-only OCCT installation (bind/libd).  Its generated OCCT
# config still names the conventional bin/lib folders, so pin the runtime path
# here instead of allowing Windows to pick up another OCCT from the system PATH.
set(OCCT_RUNTIME_DIR
    "D:/workspace/cad-algorithm-deps/occt-7.8.1/win64/vc14/bind"
    CACHE PATH "Directory containing the OCCT runtime DLLs")

set(FILLET_BASELINE_SOURCE "D:/workspace/cad-algorithm/识别圆角.cpp" CACHE FILEPATH
    "Path to the original fillet recognition baseline source")
if(NOT EXISTS "${FILLET_BASELINE_SOURCE}")
  message(FATAL_ERROR "Original baseline source not found: ${FILLET_BASELINE_SOURCE}")
endif()

add_library(fillet_search
    src/FilletBaselineAdapter.cpp
    src/ImprovedFilletRecognizer.cpp
    src/RecognitionReport.cpp
    src/TruthEvaluation.cpp
)
target_include_directories(fillet_search PUBLIC include)
target_compile_definitions(fillet_search PRIVATE
    FILLET_BASELINE_SOURCE="${FILLET_BASELINE_SOURCE}"
)
target_link_libraries(fillet_search PUBLIC
    TKBO TKBool TKBRep TKFillet TKG2d TKG3d TKGeomAlgo TKGeomBase
    TKMath TKPrim TKShHealing TKTopAlgo TKernel
)
if(MSVC)
  target_compile_options(fillet_search PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(fillet_search PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(fillet-search app/main.cpp)
target_link_libraries(fillet-search PRIVATE fillet_search TKDESTEP TKDEIGES TKXSBase)
if(MSVC)
  set_property(TARGET fillet-search PROPERTY
      VS_DEBUGGER_ENVIRONMENT "PATH=${OCCT_RUNTIME_DIR};%PATH%")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(fillet_search_tests tests/FilletBaselineTests.cpp)
  target_link_libraries(fillet_search_tests PRIVATE fillet_search)
  target_compile_definitions(fillet_search_tests PRIVATE
      FILLET_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-artifacts")
  add_test(NAME fillet_search_tests COMMAND fillet_search_tests)
  set_property(TEST fillet_search_tests PROPERTY ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${OCCT_RUNTIME_DIR}")
  if(MSVC)
    set_property(TARGET fillet_search_tests PROPERTY
        VS_DEBUGGER_ENVIRONMENT "PATH=${OCCT_RUNTIME_DIR};%PATH%")
  endif()
endif()
