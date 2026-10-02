cmake_minimum_required(VERSION 3.24)
project(FSRemoteMessages VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network WebSockets Test)
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()
add_library(message_core
    src/protocol.h
    src/keHuDuanPeiZhi.h src/keHuDuanPeiZhi.cpp # 客户端本机身份和内外网入口的文件读写。
    src/sheBeiZhuCe.h src/sheBeiZhuCe.cpp # 服务端设备登记、身份校验和永久保存。
    src/message_server.h src/message_server.cpp
    src/message_client.h src/message_client.cpp)
target_include_directories(message_core PUBLIC src)
target_link_libraries(message_core PUBLIC Qt6::Network Qt6::WebSockets)
add_library(message_ui src/main_window.h src/main_window.cpp)
target_link_libraries(message_ui PUBLIC message_core Qt6::Widgets)
add_executable(FSRemoteMessageServer src/server_main.cpp)
target_link_libraries(FSRemoteMessageServer PRIVATE message_core)
qt_add_executable(FSRemoteMessages WIN32 src/client_main.cpp resources/resources.qrc)
target_link_libraries(FSRemoteMessages PRIVATE message_ui)
enable_testing()
add_executable(message_tests tests/message_tests.cpp resources/resources.qrc)
target_link_libraries(message_tests PRIVATE message_ui Qt6::Test)
add_test(NAME messages COMMAND message_tests -o test-results.txt,txt)
set_tests_properties(messages PROPERTIES TIMEOUT 90)

# Standalone connectivity probe; does not use device credentials or message storage.
add_executable(FSRemoteNetworkDemo demo/network_demo.cpp)
target_link_libraries(FSRemoteNetworkDemo PRIVATE Qt6::Network Qt6::WebSockets)
