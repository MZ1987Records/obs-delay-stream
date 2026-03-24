if(NOT DEFINED INPUT)
  message(FATAL_ERROR "INPUT is not set")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "OUTPUT is not set")
endif()
if(NOT DEFINED VAR_NAME)
  set(VAR_NAME "kEmbeddedHtml")
endif()

string(REGEX REPLACE "^\"|\"$" "" INPUT "${INPUT}")
string(REGEX REPLACE "^\"|\"$" "" OUTPUT "${OUTPUT}")
string(REGEX REPLACE "^\"|\"$" "" VAR_NAME "${VAR_NAME}")

file(READ "${INPUT}" HTML_CONTENT)
string(REPLACE "\r\n" "\n" HTML_CONTENT "${HTML_CONTENT}")
string(REPLACE "\r" "\n" HTML_CONTENT "${HTML_CONTENT}")

get_filename_component(OUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")

file(WRITE  "${OUTPUT}" "// Generated from ${INPUT}. Do not edit.\n")
file(APPEND "${OUTPUT}" "#pragma once\n\n")
file(APPEND "${OUTPUT}" "static const char ${VAR_NAME}[] =\n")

string(REPLACE "\\" "\\\\" HTML_ESC "${HTML_CONTENT}")
string(REPLACE "\"" "\\\"" HTML_ESC "${HTML_ESC}")
# Split into concatenated C string literals per line
string(REPLACE "\n" "\\n\"\n\"" HTML_ESC "${HTML_ESC}")
file(APPEND "${OUTPUT}" "\"${HTML_ESC}\";\n")
