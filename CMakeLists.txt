cmake_minimum_required (VERSION 3.4)
project(demo)

SET(CMAKE_CXX_STANDARD 14)

if(MSVC)
add_definitions(-DUNICODE -D_UNICODE)
add_definitions(-D_WIN32_WINNT=0x0501)
endif()

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.67.0 REQUIRED COMPONENTS system thread)

# set boost include directory
include_directories(${Boost_INCLUDE_DIRS})

# set boost lib directory
link_directories(${Boost_LIBRARY_DIRS})

file(GLOB CLIENT_SRC
    src/sunny_common.h
	src/sunny_client_manager.h
	src/sunny_client_manager.cpp
    demo/client.cpp
)
file(GLOB SERVER_SRC
    src/sunny_common.h
	src/sunny_server_manager.h
	src/sunny_server_manager.cpp
    demo/server.cpp
)

add_executable(client ${CLIENT_SRC})
add_executable(server ${SERVER_SRC})
target_link_libraries(client ${Boost_LIBRARIES})
target_link_libraries(server ${Boost_LIBRARIES})
