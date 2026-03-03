cmake_minimum_required(VERSION 3.20)
project(SlipStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /GS /fp:fast")
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
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_resampler.h)
find_library(SPEEXDSP_LIBRARY speexdsp)

set(SLIPSTREAM_SOURCES
    src/main.cpp
    src/host/host_app.cpp
    src/host/core/common.cpp
    src/host/core/app_support.cpp
    src/host/net/webrtc.cpp
    src/host/io/tray.cpp
    src/host/media/audio.cpp
    src/host/io/input.cpp
    src/host/media/capture.cpp
    src/host/media/encoder.cpp
    include/host/core/common.hpp
    include/host/host_app.hpp
    include/host/core/app_support.hpp
    include/host/core/logging.hpp
    include/host/core/protocol.hpp
    include/host/core/utils.hpp
    include/host/core/audio_resampler.hpp
    include/host/core/d3d_sync.hpp
    include/host/io/tray.hpp
    include/host/media/capture.hpp
    include/host/media/encoder.hpp
    include/host/net/webrtc.hpp
    include/host/media/audio.hpp
    include/host/io/input.hpp
)

if(WIN32)
    list(APPEND SLIPSTREAM_SOURCES src/version.rc)
endif()

add_executable(SlipStream ${SLIPSTREAM_SOURCES})

target_compile_definitions(SlipStream PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(SlipStream PRIVATE ${AVCODEC_INCLUDE_DIR} ${AVUTIL_INCLUDE_DIR} ${SPEEXDSP_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(SlipStream PRIVATE LibDataChannel::LibDataChannel httplib::httplib nlohmann_json::nlohmann_json Opus::opus OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SPEEXDSP_LIBRARY})

if(WIN32)
    target_link_libraries(SlipStream PRIVATE ws2_32 d3d11 dxgi dxguid ole32 windowsapp)
    target_compile_options(SlipStream PRIVATE /await:strict /EHsc /W4 /wd4100 /wd4189)
    target_link_options(SlipStream PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    target_compile_definitions(SlipStream PRIVATE WINRT_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(SlipStream PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()

set_target_properties(SlipStream PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release)

# Copy web client files
foreach(F index.html styles.css)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/client/${F} $<TARGET_FILE_DIR:SlipStream>/${F})
endforeach()

add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SlipStream>/js)

foreach(F constants input media network renderer state ui mic auth protocol audio-worklet mic-worklet)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/client/js/${F}.js $<TARGET_FILE_DIR:SlipStream>/js/${F}.js)
endforeach()

install(TARGETS SlipStream RUNTIME DESTINATION .)
install(FILES client/index.html client/styles.css DESTINATION .)
install(DIRECTORY client/js DESTINATION .)
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/Release/ DESTINATION . FILES_MATCHING PATTERN "*.dll")
