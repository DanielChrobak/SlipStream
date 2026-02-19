cmake_minimum_required(VERSION 3.20)
project(SlipStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /GS- /fp:fast")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
endif()

find_package(LibDataChannel REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Opus REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
find_library(AVUTIL_LIBRARY avutil)

set(SLIPSTREAM_SOURCES
    src/main.cpp
    src/common.cpp
    src/app_support.cpp
    src/webrtc.cpp
    src/tray.cpp
    src/audio.cpp
    src/mic.cpp
    src/input.cpp
    src/capture.cpp
    src/encoder.cpp
    include/common.hpp
    include/app_support.hpp
    include/tray.hpp
    include/capture.hpp
    include/encoder.hpp
    include/webrtc.hpp
    include/audio.hpp
    include/input.hpp
    include/mic.hpp
)

if(WIN32)
    list(APPEND SLIPSTREAM_SOURCES src/version.rc)
endif()

add_executable(SlipStream ${SLIPSTREAM_SOURCES})

target_compile_definitions(SlipStream PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(SlipStream PRIVATE ${AVCODEC_INCLUDE_DIR} ${AVUTIL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(SlipStream PRIVATE LibDataChannel::LibDataChannel httplib::httplib nlohmann_json::nlohmann_json Opus::opus OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})

if(WIN32)
    target_link_libraries(SlipStream PRIVATE ws2_32 d3d11 dxgi dxguid ole32 windowsapp)
    target_compile_options(SlipStream PRIVATE /await:strict /EHsc /W4 /wd4100 /wd4189)
    target_compile_definitions(SlipStream PRIVATE WINRT_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(SlipStream PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()

set_target_properties(SlipStream PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release)

# Copy web client files
foreach(F index.html styles.css)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/client/${F} $<TARGET_FILE_DIR:SlipStream>/${F})
endforeach()

add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SlipStream>/js)

foreach(F input media network renderer state ui mic)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/client/js/${F}.js $<TARGET_FILE_DIR:SlipStream>/js/${F}.js)
endforeach()

install(TARGETS SlipStream RUNTIME DESTINATION .)
install(FILES client/index.html client/styles.css DESTINATION .)
install(DIRECTORY client/js DESTINATION .)
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/Release/ DESTINATION . FILES_MATCHING PATTERN "*.dll")
