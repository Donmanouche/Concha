# The Strudel runtime (js/src/concha.mjs + third_party/strudel) bundled by esbuild, then compiled
# into the binary, and the C++ pattern engine that runs it in QuickJS-ng.
# Needs node at build time (not at run time). `npm --prefix js ci` installs esbuild and Strudel's
# npm dependencies (js/package-lock.json).

find_program(CONCHA_NODE node REQUIRED)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/quickjs-ng ${PROJECT_BINARY_DIR}/quickjs-ng EXCLUDE_FROM_ALL)

file(GLOB_RECURSE concha_js_inputs CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/js/src/*.mjs ${PROJECT_SOURCE_DIR}/js/src/*.json
    ${PROJECT_SOURCE_DIR}/third_party/strudel/packages/*.mjs)
list(FILTER concha_js_inputs EXCLUDE REGEX "/node_modules/|/test/|/dist/")

set(CONCHA_BUNDLE ${PROJECT_BINARY_DIR}/js/concha-strudel.js)
add_custom_command(
    OUTPUT ${CONCHA_BUNDLE}
    COMMAND ${CONCHA_NODE} ${PROJECT_SOURCE_DIR}/js/build.mjs ${PROJECT_SOURCE_DIR}/js/src/concha.mjs ${CONCHA_BUNDLE}
    DEPENDS ${concha_js_inputs} ${PROJECT_SOURCE_DIR}/js/build.mjs
    COMMENT "Bundling Strudel (esbuild)"
    VERBATIM)

add_executable(concha_embed tools/embed/embed.cpp)
add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/js/EmbeddedBundle.cpp
    COMMAND concha_embed ${CONCHA_BUNDLE} ${PROJECT_BINARY_DIR}/js/EmbeddedBundle.cpp concha embeddedBundle
    DEPENDS concha_embed ${CONCHA_BUNDLE}
    COMMENT "Embedding the Strudel bundle"
    VERBATIM)
add_custom_target(concha_js DEPENDS ${CONCHA_BUNDLE})

# Tidal's Haskell parser (hs2js): tree-sitter's C runtime and tree-sitter-haskell, see HaskellParser.h
add_library(concha_treesitter STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/src/lib.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-haskell/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-haskell/src/scanner.c)
target_include_directories(concha_treesitter
    PUBLIC ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/src ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-haskell/src)
set_target_properties(concha_treesitter PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
target_compile_options(concha_treesitter PRIVATE -w)

add_library(concha_pattern STATIC
    src/pattern/PatternEngine.cpp
    src/pattern/HaskellParser.cpp
    src/input/Gamepad.cpp
    ${PROJECT_BINARY_DIR}/js/EmbeddedBundle.cpp)
target_include_directories(concha_pattern PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(concha_pattern PRIVATE qjs concha_treesitter)
