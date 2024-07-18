cmake_minimum_required(VERSION 3.20)

project(WebRTCConnector)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES x86_64;arm64)
add_subdirectory(proxy_connector)
# add_subdirectory(proxy_sender)
# add_subdirectory(proxy_receiver)

