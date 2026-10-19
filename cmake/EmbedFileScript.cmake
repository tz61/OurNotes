file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _size "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _bytes "${_bytes}")
file(WRITE "${OUT}" "#include <cstddef>\n"
    "extern const unsigned char ${SYMBOL}[] = {\n${_bytes}0x00};\n"
    "extern const std::size_t ${SYMBOL}_size = ${_size};\n")
