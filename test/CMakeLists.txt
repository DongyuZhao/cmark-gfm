# To get verbose output: cmake --build build --target "test" -- ARGS='-V'

# By default, we run the spec tests only if python3 is available.
# To require the spec tests, compile with -DSPEC_TESTS=1

if (SPEC_TESTS)
  find_package(PythonInterp 3 REQUIRED)
else(SPEC_TESTS)
  find_package(PythonInterp 3)
endif(SPEC_TESTS)

if (CMARK_SHARED OR CMARK_STATIC)
  add_test(NAME api_test COMMAND api_test)
endif()

if (WIN32)
  file(TO_NATIVE_PATH ${CMAKE_BINARY_DIR}/src WIN_SRC_DLL_DIR)
  file(TO_NATIVE_PATH ${CMAKE_BINARY_DIR}/extensions WIN_EXTENSIONS_DLL_DIR)
  set(NEWPATH "${WIN_SRC_DLL_DIR};${WIN_EXTENSIONS_DLL_DIR};$ENV{PATH}")
  string(REPLACE ";" "\\;" NEWPATH "${NEWPATH}")
  set_tests_properties(api_test PROPERTIES ENVIRONMENT "PATH=${NEWPATH}")
  set(ROUNDTRIP "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.bat")
else(WIN32)
  set(ROUNDTRIP "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.sh")
endif(WIN32)

IF (PYTHONINTERP_FOUND)

  add_test(html_normalization
    ${PYTHON_EXECUTABLE} "-m" "doctest"
    "${CMAKE_CURRENT_SOURCE_DIR}/normalize.py"
    )

  if (CMARK_SHARED)
    add_test(spectest_library
      ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py" "--no-normalize" "--spec"
      "${CMAKE_CURRENT_SOURCE_DIR}/spec.txt" "--library-dir" "${CMAKE_CURRENT_BINARY_DIR}/../src"
      )

    add_test(pathological_tests_library
      ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/pathological_tests.py"
      "--library-dir" "${CMAKE_CURRENT_BINARY_DIR}/../src"
      )

    add_test(roundtriptest_library
      ${PYTHON_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_tests.py"
      "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/spec.txt"
      "--library-dir" "${CMAKE_CURRENT_BINARY_DIR}/../src"
      )

    add_test(entity_library
      ${PYTHON_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/entity_tests.py"
      "--library-dir" "${CMAKE_CURRENT_BINARY_DIR}/../src"
      )
  endif()

  add_test(spectest_executable
    ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py" "--no-normalize" "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/spec.txt" "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm"
    )

  add_test(smartpuncttest_executable
    ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py" "--no-normalize" "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/smart_punct.txt" "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --smart"
    )

  add_test(extensions_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm"
    "--extensions" "table strikethrough autolink tagfilter footnotes tasklist"
    )

  add_test(formula_github_compatibility_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-formula-github.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm"
    "--extensions" "formula"
    )

  add_test(formula_latex_compatibility_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-formula-latex.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --latex-formula-delimiters"
    "--extensions" "formula"
    )

  add_test(formula_ms_compatibility_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-formula-ms.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --ms-formula-delimiters"
    "--extensions" "formula"
    )

  add_test(formula_conflicts_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-formula-conflicts.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --latex-formula-delimiters"
    "--extensions" "formula"
    )

  add_test(ms_copilot_citation_extension_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-ms-copilot-citation.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm -t xml --ms-copilot-citation"
    )

  add_test(inline_delimiter_stack_tests_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/inline_delimiter_stack_tests.py"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --latex-formula-delimiters --ms-copilot-annotation --ms-copilot-citation"
    )

  add_test(ms_copilot_accordion_extension_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-ms-copilot-accordion.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm -t xml --ms-copilot-accordion"
    )

  add_test(ms_copilot_annotation_extension_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-ms-copilot-annotation.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm -t xml --ms-copilot-annotation"
    )

  add_test(ms_copilot_annotation_render_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/ms_copilot_annotation_render_tests.py"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --ms-copilot-annotation"
    )

  add_test(ms_copilot_option_gates_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py"
    "--no-normalize"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-ms-copilot-option-gates.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm -t xml"
    "--extensions" "ms_copilot_accordion ms_copilot_annotation ms_copilot_citation"
    )

  add_test(ms_copilot_accordion_depth_limit_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/ms_copilot_accordion_depth_limit.py"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --ms-copilot-accordion"
    )

  add_test(roundtrip_extensions_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_tests.py"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm"
    "--extensions" "table strikethrough autolink tagfilter footnotes tasklist"
    )

  add_test(option_table_prefer_style_attributes
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_tests.py"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-table-prefer-style-attributes.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --table-prefer-style-attributes"
    "--extensions" "table strikethrough autolink tagfilter footnotes tasklist"
    )

  add_test(option_full_info_string
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_tests.py"
    "--spec" "${CMAKE_CURRENT_SOURCE_DIR}/extensions-full-info-string.txt"
    "--program" "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm --full-info-string"
    )

  add_test(regressiontest_executable
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/spec_tests.py" "--no-normalize" "--spec"
    "${CMAKE_CURRENT_SOURCE_DIR}/regression.txt" "--program"
    "${CMAKE_CURRENT_BINARY_DIR}/../src/cmark-gfm"
    )


ELSE(PYTHONINTERP_FOUND)

  message("\n*** A python 3 interpreter is required to run the spec tests.\n")
  add_test(skipping_spectests
    echo "Skipping spec tests, because no python 3 interpreter is available.")

ENDIF(PYTHONINTERP_FOUND)
