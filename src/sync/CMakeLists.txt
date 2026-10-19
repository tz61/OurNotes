add_library(freepen_sync STATIC
    store.cpp
    local_transport.cpp
    http_transport.cpp
    server_api.cpp
)
target_link_libraries(freepen_sync PUBLIC freepen_core fp_httplib)
