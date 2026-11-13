# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" body "${body}")
file(WRITE "${OUTPUT}" "// generated from ${INPUT}\n#include <cstddef>\nextern const unsigned char ${SYMBOL}[] = {\n${body}\n};\nextern const std::size_t ${SYMBOL}Size = ${bytes};\n")
