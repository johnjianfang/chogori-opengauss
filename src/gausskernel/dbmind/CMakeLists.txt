#This is the main CMAKE for build bin.

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_RULE_MESSAGES OFF)
set(CMAKE_SKIP_RPATH TRUE)

INCLUDE_DIRECTORIES(${LIBTHRIFT_INCLUDE_PATH} ${SNAPPY_INCLUDE_PATH} ${CJSON_INCLUDE_PATH} ${BOOST_INCLUDE_PATH})

set(CMAKE_MODULE_PATH 
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/db4ai
)

add_subdirectory(kernel)
add_subdirectory(db4ai)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools/ DESTINATION bin/dbmind)
