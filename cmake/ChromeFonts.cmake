# Clearwater chrome fonts (DESIGN.md "Type"): one baked atlas per type role and
# desktop scale, plus the line-icon masks. Shared by the kernel and the chrome
# golden test so both rasterise the same glyphs. The sets are defined in
# src/kernel/src/chrome_fonts.cpp (ChromeFontSet, chrome_raster.h).
#
# brook_chrome_fonts(<out-var> <python3> <regular.ttf> <mono.ttf> <out-dir>)
# appends the generated sources and chrome_fonts.cpp to <out-var>.
function(brook_chrome_fonts OUT_VAR PYTHON3 REGULAR_TTF MONO_TTF OUT_DIR)
    get_filename_component(REGULAR_DIR "${REGULAR_TTF}" DIRECTORY)
    get_filename_component(REGULAR_NAME "${REGULAR_TTF}" NAME_WE)
    set(BOLD_TTF "${REGULAR_DIR}/${REGULAR_NAME}-Bold.ttf")
    if(NOT EXISTS "${BOLD_TTF}")
        set(BOLD_TTF "${REGULAR_TTF}")
    endif()
    if(NOT MONO_TTF OR NOT EXISTS "${MONO_TTF}")
        set(MONO_TTF "${REGULAR_TTF}")
    endif()
    # Role, logical pixel size at 100%, face. Keep in step with ChromeFont.
    set(ROLES "Body:13:${REGULAR_TTF}" "Label:12:${REGULAR_TTF}" "Small:11:${REGULAR_TTF}"
              "Strong:13:${BOLD_TTF}" "Lead:15:${BOLD_TTF}" "Hero:20:${REGULAR_TTF}" "Mono:11:${MONO_TTF}")
    set(SOURCES "")
    foreach(SCALE IN ITEMS 100 150 200)
        foreach(ROLE IN LISTS ROLES)
            string(REPLACE ":" ";" PARTS "${ROLE}")
            list(GET PARTS 0 NAME)
            list(GET PARTS 1 BASE)
            list(GET PARTS 2 TTF)
            math(EXPR SIZE "(${BASE} * ${SCALE} + 50) / 100")
            set(CPP "${OUT_DIR}/chrome_font_${NAME}_${SCALE}.cpp")
            add_custom_command(
                OUTPUT "${CPP}"
                COMMAND ${PYTHON3} "${BROOK_ROOT}/scripts/bake_font.py" "${TTF}" "${SIZE}" "${CPP}"
                        "g_chrome${NAME}${SCALE}" "b0,b7"
                DEPENDS "${BROOK_ROOT}/scripts/bake_font.py" "${TTF}"
                COMMENT "Baking chrome font ${NAME} (${SCALE}%)"
                VERBATIM)
            list(APPEND SOURCES "${CPP}")
        endforeach()
        math(EXPR ICON_SIZE "16 * ${SCALE} / 100")
        set(CPP "${OUT_DIR}/chrome_font_Icons_${SCALE}.cpp")
        add_custom_command(
            OUTPUT "${CPP}"
            COMMAND ${PYTHON3} "${BROOK_ROOT}/scripts/bake_icons.py" "${ICON_SIZE}" "${CPP}" "g_chromeIcons${SCALE}"
            DEPENDS "${BROOK_ROOT}/scripts/bake_icons.py"
            COMMENT "Baking chrome icons (${SCALE}%)"
            VERBATIM)
        list(APPEND SOURCES "${CPP}")
    endforeach()
    list(APPEND SOURCES "${BROOK_ROOT}/src/kernel/src/chrome_fonts.cpp")
    set(${OUT_VAR} ${${OUT_VAR}} ${SOURCES} PARENT_SCOPE)
endfunction()
