# komi-tube native programs, included from vendor/tilefinch/CMakeLists.txt
# through KOMI_EXTRA_CMAKE so they link the same tilefinch_core and transport
# the browser EBOOT does. Each program gets <build>/<name>/ holding
#   <name>.prx   loaded from host0: by PSPLink (scripts/run_native.sh)
#   EBOOT.PBP    the same program for the Memory Stick
# plus the files it reads beside itself (roots.pem, fonts/, ...).
if(NOT PSP OR NOT PSP_BROWSER_LIBCURL_TRANSPORT OR PSP_BROWSER_CURL_STUB)
    return()
endif()

set(KOMI_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The media session and its presenter live in the browser executable's own
# source list, not in tilefinch_core; every program compiles the same files.
set(KOMI_RUNTIME_SOURCES
    "${KOMI_NATIVE_DIR}/common/komi_runtime.c"
    "${KOMI_NATIVE_DIR}/common/psp_log_enabled.c"
    src/psp_media_buffering.c
    src/psp_media_open.c
    src/psp_media_hls.c
    src/psp_hls_gzip.c
    src/psp_media_present_session.c
    src/psp_media_seek.c
    src/psp_media_session.c
    src/psp_media_telemetry.c
    src/psp_swdec_component.c
    src/psp_network.c
    src/psp_time.c
    src/psp_atomic_shims.c)

# komi_program(<name> <title> SOURCES ... [FILES dest=src ...])
function(komi_program name title)
    cmake_parse_arguments(ARG "" "" "SOURCES;FILES" ${ARGN})
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    file(MAKE_DIRECTORY "${out}")
    foreach(kind elf prx)
        set(target ${name}-${kind})
        add_executable(${target} ${KOMI_RUNTIME_SOURCES} ${ARG_SOURCES})
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME ${name}-${kind}.elf
            RUNTIME_OUTPUT_DIRECTORY "${out}")
        target_include_directories(${target} PRIVATE
            src "${KOMI_NATIVE_DIR}/common" "${KOMI_NATIVE_DIR}/app")
        # TILEFINCH_PSP_LOG_IMPLEMENTATION keeps psp_log_* real in these
        # sources (shipping builds macro them away) without
        # TILEFINCH_PSP_VALIDATION_LOG, which would change struct layouts
        # shared with tilefinch_core.
        target_compile_definitions(${target} PRIVATE
            TILEFINCH_PSP_LIVE_NETWORK=1 TILEFINCH_PSP_LOG_IMPLEMENTATION=1)
        target_link_options(${target} PRIVATE
            "LINKER:--wrap=printf" "LINKER:--wrap=memalign")
        target_link_libraries(${target} PRIVATE
            tilefinch_core tilefinch_psp_ui tilefinch_psp_display
            tilefinch_psp_media_scale tilefinch_psp_media_present
            tilefinch_psp_app_support
            ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
            "-Wl,--whole-archive"
            pspnet pspnet_inet pspnet_apctl pspnet_resolver pspwlan psputility
            "-Wl,--no-whole-archive"
            m pspdisplay pspge pspctrl pspgu pspdmac psppower
            pspaudiocodec pspaudio psputility)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
            set_property(TARGET ${target} PROPERTY
                LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
        endif()
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${PSP_BROWSER_GC_KEEP_SCRIPT}")
    endforeach()

    # PRX: the PSPSDK relocatable-module recipe, as psp-browser-script-dev-prx.
    target_link_options(${name}-prx PRIVATE
        "-specs=${PSPDEV}/psp/sdk/lib/prxspecs"
        "LINKER:-q"
        "LINKER:-T,${PSPDEV}/psp/sdk/lib/linkfile.prx"
        "${PSPDEV}/psp/sdk/lib/prxexports.o")
    set(copies
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PSP_BROWSER_PSP_CA_BUNDLE}" "${out}/roots.pem")
    foreach(pair IN LISTS ARG_FILES)
        string(REPLACE "=" ";" parts "${pair}")
        list(GET parts 0 dest)
        list(GET parts 1 source)
        get_filename_component(dest_dir "${out}/${dest}" DIRECTORY)
        list(APPEND copies
            COMMAND ${CMAKE_COMMAND} -E make_directory "${dest_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${source}" "${out}/${dest}")
    endforeach()
    add_custom_command(TARGET ${name}-prx POST_BUILD
        COMMAND "${PSPDEV}/bin/psp-fixup-imports" "${out}/${name}-prx.elf"
        COMMAND "${PSPDEV}/bin/psp-prxgen" "${out}/${name}-prx.elf"
            "${out}/${name}.prx"
        ${copies}
        COMMENT "Generating ${name}.prx for PSPLink")

    # EBOOT: same sources, packed for the Memory Stick. MEMSIZE=1 is patched
    # in by scripts/package.py's set_pbp_title when it is installed. Strip it
    # first (keeping .unstripped symbols), as the browser EBOOT does: this
    # preset is MinSizeRel, which the SDK's own strip step skips.
    add_custom_command(TARGET ${name}-elf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${out}/${name}-elf.elf"
            "${out}/${name}-elf.elf.unstripped"
        COMMAND "${PSPDEV}/bin/psp-strip" "${out}/${name}-elf.elf"
        COMMENT "Stripping ${name} (keeping .unstripped symbols)")
    create_pbp_file(TARGET ${name}-elf TITLE "${title}" OUTPUT_DIR "${out}")
endfunction()

komi_program(komi-player "komi-player"
    SOURCES "${KOMI_NATIVE_DIR}/player/main.c"
    FILES "komi-videos.txt=${KOMI_NATIVE_DIR}/player/videos.txt")

komi_program(komi-app "komi-tube"
    SOURCES
        "${KOMI_NATIVE_DIR}/app/main.c"
        "${KOMI_NATIVE_DIR}/app/kanji.c"
        "${KOMI_NATIVE_DIR}/app/mylist.c"
        "${KOMI_NATIVE_DIR}/app/osk.c"
        "${KOMI_NATIVE_DIR}/app/search.c"
        "${KOMI_NATIVE_DIR}/app/ui.c"
        "${KOMI_NATIVE_DIR}/app/yt_results.c"
    FILES
        "fonts/TilefinchSans-Regular.ttf=${CMAKE_CURRENT_SOURCE_DIR}/fonts/TilefinchSans-Regular.ttf"
        "fonts/LICENSE-TilefinchSans.txt=${CMAKE_CURRENT_SOURCE_DIR}/fonts/LICENSE-TilefinchSans.txt"
        "fonts/LICENSE-Unifont.txt=${CMAKE_CURRENT_SOURCE_DIR}/fonts/LICENSE-Unifont.txt")

# dopagaki-portable: Shorts on a PSP held upright (native/dopagaki).
komi_program(dopagaki "dopagaki-portable"
    SOURCES
        "${KOMI_NATIVE_DIR}/dopagaki/main.c"
        "${KOMI_NATIVE_DIR}/dopagaki/shorts.c"
        "${KOMI_NATIVE_DIR}/dopagaki/shorts_parse.c"
        "${KOMI_NATIVE_DIR}/app/kanji.c"
        "${KOMI_NATIVE_DIR}/app/mylist.c"
        "${KOMI_NATIVE_DIR}/app/osk.c"
        "${KOMI_NATIVE_DIR}/app/ui.c"
        "${KOMI_NATIVE_DIR}/app/yt_results.c"
    FILES
        "fonts/TilefinchSans-Regular.ttf=${CMAKE_CURRENT_SOURCE_DIR}/fonts/TilefinchSans-Regular.ttf"
        "fonts/LICENSE-TilefinchSans.txt=${CMAKE_CURRENT_SOURCE_DIR}/fonts/LICENSE-TilefinchSans.txt"
        "fonts/LICENSE-Unifont.txt=${CMAKE_CURRENT_SOURCE_DIR}/fonts/LICENSE-Unifont.txt")
