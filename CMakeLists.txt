file(GLOB HEADERS "include/eosio/query_api_plugin/*.hpp")
add_library( query_api_plugin
             query_api_plugin.cpp
             ${HEADERS} )

target_link_libraries( query_api_plugin appbase fc chain_plugin http_plugin )
target_include_directories( query_api_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
