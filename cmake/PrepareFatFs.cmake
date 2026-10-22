include_guard(GLOBAL)
# Retain an unmodified upstream copy and compile a single wrapper translation
# unit, so the object adapter shares FatFs internals/configuration without
# modifying the submodule or maintaining a second implementation.
function(brook_prepare_fatfs destination)
    set(source "${BROOK_ROOT}/src/third_party/fatfs/source")
    file(MAKE_DIRECTORY "${destination}")
    foreach(name ff.h ffsystem.c ffunicode.c diskio.h)
        configure_file("${source}/${name}" "${destination}/${name}" COPYONLY)
    endforeach()
    configure_file("${source}/ff.c" "${destination}/ff_upstream.c" COPYONLY)
    configure_file("${BROOK_ROOT}/src/kernel/src/ffconf.h" "${destination}/ffconf.h" COPYONLY)
    configure_file("${BROOK_ROOT}/src/kernel/src/fatfs_objects.c.in" "${destination}/ff.c" @ONLY)
endfunction()
