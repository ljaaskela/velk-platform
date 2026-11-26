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
#
# A line that is only `#velk_include MACRO` puts that C++ macro, another shader text's, in its
# place: the file is then text pieces between such lines, and each text piece is preceded by one
# newline, as if each piece were a file of its own. So the lines `#velk_include A` and
# `#velk_include B` between two pieces give the first piece's text, A, B, a newline and the second
# piece's text; the marker lines' own newlines are not in the text.

if(NOT INPUT OR NOT OUTPUT OR NOT NAME)
    message(FATAL_ERROR "embed_glsl.cmake needs INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)

get_filename_component(source_name "${INPUT}" NAME)
set(text "// Generated from ${source_name} by embed_glsl.cmake: edit the .glsl, not this.\n")
string(APPEND text "#define ${NAME}")

# The literal of the line being read, without its quotes, and the line as it is when it has only
# printable characters (`plain`), to tell a marker line. Built as plain text, not a CMake list: a
# list would not split at a semicolon between square brackets.
set(line "")
set(raw "")
set(plain TRUE)
# Whether the next text line starts a piece, and so takes the piece's newline first.
set(piece_start TRUE)
set(emitted FALSE)
set(i 0)
while(i LESS hex_length)
    string(SUBSTRING "${hex}" ${i} 2 byte)
    math(EXPR i "${i} + 2")
    math(EXPR code "0x${byte}")
    if(code EQUAL 13)
        # A carriage return: dropped.
    elseif(code EQUAL 10)
        if(plain AND raw MATCHES "^#velk_include ([A-Za-z_][A-Za-z0-9_]*)$")
            string(APPEND text " \\\n    ${CMAKE_MATCH_1}")
            set(piece_start TRUE)
        else()
            if(piece_start)
                string(APPEND text " \\\n    \"\\n\"")
                set(piece_start FALSE)
            endif()
            string(APPEND text " \\\n    \"${line}\\n\"")
        endif()
        set(emitted TRUE)
        set(line "")
        set(raw "")
        set(plain TRUE)
    elseif(code EQUAL 92 OR code EQUAL 34 OR code LESS 32 OR code GREATER 126)
        math(EXPR d2 "${code} / 64")
        math(EXPR d1 "(${code} / 8) % 8")
        math(EXPR d0 "${code} % 8")
        string(APPEND line "\\${d2}${d1}${d0}")
        set(plain FALSE)
    else()
        string(ASCII ${code} c)
        string(APPEND line "${c}")
        string(APPEND raw "${c}")
    endif()
endwhile()
if(NOT line STREQUAL "")
    if(plain AND raw MATCHES "^#velk_include ([A-Za-z_][A-Za-z0-9_]*)$")
        string(APPEND text " \\\n    ${CMAKE_MATCH_1}")
    else()
        if(piece_start)
            string(APPEND text " \\\n    \"\\n\"")
        endif()
        string(APPEND text " \\\n    \"${line}\"")
    endif()
    set(emitted TRUE)
endif()
if(NOT emitted)
    # An empty file: its newline alone, as a raw literal of nothing on its own line was.
    string(APPEND text " \\\n    \"\\n\"")
endif()
string(APPEND text "\n")
file(WRITE "${OUTPUT}" "${text}")
