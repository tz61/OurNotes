# on_embed_file(<target> <file> <symbol>)
#
# Compiles <file> into <target> as `extern const unsigned char <symbol>[]` with
# `extern const std::size_t <symbol>_size`. Used for schema.json so the core
# library does not depend on Qt resources.

function(on_embed_file target file symbol)
    get_filename_component(_abs "${file}" ABSOLUTE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/embedded_${symbol}.cpp")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -DIN=${_abs} -DOUT=${_out} -DSYMBOL=${symbol}
                -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFileScript.cmake"
        DEPENDS "${_abs}" "${PROJECT_SOURCE_DIR}/cmake/EmbedFileScript.cmake"
        COMMENT "Embedding ${file}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_out}")
endfunction()
