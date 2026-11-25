# Turns a GLSL source into a C++ header that defines it as a string literal macro, for the shader
# text to be built into the library: no file is read at runtime.
#
#   cmake -DINPUT=<file.glsl> -DOUTPUT=<file.inc> -DNAME=<MACRO> -P embed_glsl.cmake
#
# The header defines NAME as one ordinary string literal a source line, joined by adjacency, so
# the macro expands to the file's bytes exactly, preceded by one newline (as a raw literal opened
# with R"( on its own line was). Carriage returns are dropped, as the compiler drops them from a
# raw literal in a CRLF file. Backslashes, double quotes, and every byte below 0x20 or from 0x7f on
# but the newline are written as octal escapes, so nothing depends on the file's encoding. A file
# that does not end in a newline gives a macro that does not either.

if(NOT INPUT OR NOT OUTPUT OR NOT NAME)
    message(FATAL_ERROR "embed_glsl.cmake needs INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)

get_filename_component(source_name "${INPUT}" NAME)
set(text "// Generated from ${source_name} by embed_glsl.cmake: edit the .glsl, not this.\n")
string(APPEND text "#define ${NAME} \\\n    \"\\n\"")

# The literal of the line being read, without its quotes. Built as plain text, not a CMake list:
# a list would not split at a semicolon between square brackets.
set(line "")
set(i 0)
while(i LESS hex_length)
    string(SUBSTRING "${hex}" ${i} 2 byte)
    math(EXPR i "${i} + 2")
    math(EXPR code "0x${byte}")
    if(code EQUAL 13)
        # A carriage return: dropped.
    elseif(code EQUAL 10)
        string(APPEND text " \\\n    \"${line}\\n\"")
        set(line "")
    elseif(code EQUAL 92 OR code EQUAL 34 OR code LESS 32 OR code GREATER 126)
        math(EXPR d2 "${code} / 64")
        math(EXPR d1 "(${code} / 8) % 8")
        math(EXPR d0 "${code} % 8")
        string(APPEND line "\\${d2}${d1}${d0}")
    else()
        string(ASCII ${code} c)
        string(APPEND line "${c}")
    endif()
endwhile()
if(NOT line STREQUAL "")
    string(APPEND text " \\\n    \"${line}\"")
endif()
string(APPEND text "\n")
file(WRITE "${OUTPUT}" "${text}")
