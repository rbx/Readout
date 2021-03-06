# @author  Barthélémy von Haller

include(FindDoxygen)

if(NOT DOXYGEN_DOT_FOUND)
    message(WARNING "Graphviz doesn't seem to be installed. Doxygen will not be able to generate graphs. Consider installing this package.")
endif(NOT DOXYGEN_DOT_FOUND)

if (DOXYGEN_FOUND)
    # Configure the doxygen config file with current settings
    set("DOC_OUTPUT_DIR" "${CMAKE_CURRENT_BINARY_DIR}")
    configure_file(doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/documentation-config.doxygen @ONLY)

    # target doc
    add_custom_target(doc
            ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/documentation-config.doxygen
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation using doxygen for ${PROJECT_NAME}
                     \n  Output will be available in ${DOC_OUTPUT_DIR}/html" VERBATIM)

    # installation
    option(DOC_INSTALL "Install the documentation when calling \"make install\"" OFF)
    if(DOC_INSTALL)
        message(STATUS "Documentation will be installed but you *must* run `make doc`")
        install(DIRECTORY ${DOC_OUTPUT_DIR}/html DESTINATION share/doc/${PROJECT_NAME} COMPONENT doc)
    endif(DOC_INSTALL)
endif (DOXYGEN_FOUND)
