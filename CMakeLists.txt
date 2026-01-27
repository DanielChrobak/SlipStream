cmake_minimum_required(VERSION 3.20)
project(SlipStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

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
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
find_library(AVUTIL_LIBRARY avutil)

add_executable(SlipStream main.cpp hpp/common.hpp hpp/capture.hpp hpp/encoder.hpp hpp/webrtc.hpp hpp/audio.hpp hpp/input.hpp)

target_compile_definitions(SlipStream PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(SlipStream PRIVATE ${AVCODEC_INCLUDE_DIR} ${AVUTIL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/hpp)
target_link_libraries(SlipStream PRIVATE LibDataChannel::LibDataChannel httplib::httplib nlohmann_json::nlohmann_json Opus::opus OpenSSL::SSL OpenSSL::Crypto ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})

if(WIN32)
    target_link_libraries(SlipStream PRIVATE ws2_32 d3d11 dxgi dxguid ole32 windowsapp)
    target_compile_options(SlipStream PRIVATE /await:strict /EHsc /W4 /wd4100 /wd4189)
    target_compile_definitions(SlipStream PRIVATE WINRT_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(SlipStream PRIVATE "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
endif()

set_target_properties(SlipStream PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release)

foreach(F index.html styles.css)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/${F} $<TARGET_FILE_DIR:SlipStream>/${F})
endforeach()

add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SlipStream>/js)

foreach(F input media network renderer state ui)
    add_custom_command(TARGET SlipStream POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/js/${F}.js $<TARGET_FILE_DIR:SlipStream>/js/${F}.js)
endforeach()

install(TARGETS SlipStream RUNTIME DESTINATION .)
install(FILES index.html styles.css DESTINATION .)
install(DIRECTORY js DESTINATION .)
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/Release/ DESTINATION . FILES_MATCHING PATTERN "*.dll")

set(CPACK_PACKAGE_NAME "SlipStream")
set(CPACK_PACKAGE_VENDOR "SlipStream")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency remote desktop streaming with AV1 encoding")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SlipStream")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")
set(CPACK_NSIS_DISPLAY_NAME "SlipStream")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/icon.ico")
endif()

set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\SlipStream.lnk' '$INSTDIR\\\\SlipStream.exe'")
set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\SlipStream.lnk'")
set(CPACK_GENERATOR "ZIP")
include(CPack)
