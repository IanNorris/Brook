# Clearwater chrome fonts (DESIGN.md "Type"): one baked atlas per type role and
# desktop scale, plus the line-icon masks. Shared by the kernel and the chrome
# golden test so both rasterise the same glyphs. Bake each distinct pixel
# size once per font role and reuse it across adjacent scale settings.
#
# brook_chrome_fonts(<out-var> <python3> <regular.ttf> <mono.ttf> <out-dir>)
# appends the generated sources to <out-var>.
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
    set(ATLASES "")
    set(DECLARATIONS "")
    set(SETS "")
    set(TABLE "")
    foreach(SCALE RANGE 100 200 5)
        set(SET "")
        foreach(ROLE IN LISTS ROLES)
            string(REPLACE ":" ";" PARTS "${ROLE}")
            list(GET PARTS 0 NAME)
            list(GET PARTS 1 BASE)
            list(GET PARTS 2 TTF)
            math(EXPR SIZE "(${BASE} * ${SCALE} + 50) / 100")
            set(SYMBOL "g_chrome${NAME}Px${SIZE}")
            if(NOT SYMBOL IN_LIST ATLASES)
                set(CPP "${OUT_DIR}/chrome_font_${NAME}_px${SIZE}.cpp")
                add_custom_command(
                    OUTPUT "${CPP}"
                    COMMAND ${PYTHON3} "${BROOK_ROOT}/scripts/bake_font.py" "${TTF}" "${SIZE}" "${CPP}"
                            "${SYMBOL}" "b0,b7,2013,2014"
                    DEPENDS "${BROOK_ROOT}/scripts/bake_font.py" "${TTF}"
                    COMMENT "Baking chrome font ${NAME} (${SIZE}px)"
                    VERBATIM)
                list(APPEND SOURCES "${CPP}")
                list(APPEND ATLASES "${SYMBOL}")
                string(APPEND DECLARATIONS "extern const FontAtlas ${SYMBOL};\n")
            endif()
            string(APPEND SET "&${SYMBOL}, ")
        endforeach()
        math(EXPR ICON_SIZE "(16 * ${SCALE} + 50) / 100")
        set(SYMBOL "g_chromeIconPx${ICON_SIZE}")
        if(NOT SYMBOL IN_LIST ATLASES)
            set(CPP "${OUT_DIR}/chrome_icons_px${ICON_SIZE}.cpp")
            add_custom_command(
                OUTPUT "${CPP}"
                COMMAND ${PYTHON3} "${BROOK_ROOT}/scripts/bake_icons.py" "${ICON_SIZE}" "${CPP}" "${SYMBOL}"
                DEPENDS "${BROOK_ROOT}/scripts/bake_icons.py"
                COMMENT "Baking chrome icons (${ICON_SIZE}px)"
                VERBATIM)
            list(APPEND SOURCES "${CPP}")
            list(APPEND ATLASES "${SYMBOL}")
            string(APPEND DECLARATIONS "extern const FontAtlas ${SYMBOL};\n")
        endif()
        string(APPEND SETS "extern const ChromeFontSet g_chromeFonts${SCALE} = {{${SET}&${SYMBOL}}};\n")
        string(APPEND TABLE "    &g_chromeFonts${SCALE},\n")
    endforeach()
    set(REGISTRY "${OUT_DIR}/chrome_font_sets.cpp")
    file(GENERATE OUTPUT "${REGISTRY}" CONTENT
"#include \"chrome_raster.h\"\n#include \"desktop_metrics.h\"\nnamespace brook {\n${DECLARATIONS}${SETS}
const ChromeFontSet& ChromeFontsForScale(uint32_t percent) {
    static constexpr const ChromeFontSet* sets[] = {
${TABLE}    };
    static_assert(sizeof(sets)/sizeof(*sets) == BROOK_DESKTOP_SCALE_COUNT);
    return *sets[brook_desktop_scale_valid(percent) ?
        (percent-BROOK_DESKTOP_SCALE_MIN)/BROOK_DESKTOP_SCALE_STEP : 0];
}
}\n")
    list(APPEND SOURCES "${REGISTRY}")
    set(${OUT_VAR} ${${OUT_VAR}} ${SOURCES} PARENT_SCOPE)
endfunction()
