add_library(ournotes_sync STATIC
    store.cpp
    local_transport.cpp
    http_transport.cpp
    server_api.cpp
)
target_link_libraries(ournotes_sync PUBLIC ournotes_core on_httplib)
