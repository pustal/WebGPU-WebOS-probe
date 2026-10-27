# WebOSPackage.cmake
#
# Adds the "webos-package" target: stages the IPK layout under <build>/ipk/
#   appinfo.json, icons, bin/wgpuprobe, lib/libSDL2-2.0.so.0,
#   lib/libwebgpu_dawn.so, licences
# strips the binary and builds dist/<id>_<version>_arm.ipk with ares-package
# (from @webos-tools/cli), plus the Homebrew Channel manifest next to it.

find_program(PROBE_ARES_PACKAGE ares-package)

set(PROBE_IPK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ipk")
set(PROBE_DIST_DIR "${PROBE_ROOT}/dist")
set(PROBE_IPK_NAME "${PROBE_APP_ID}_${PROJECT_VERSION}_arm.ipk")

configure_file("${PROBE_ROOT}/webos/appinfo.json.in" "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" @ONLY)

if(NOT CMAKE_STRIP)
    set(CMAKE_STRIP strip)
endif()

add_custom_target(webos-stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PROBE_IPK_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROBE_IPK_STAGE}/bin" "${PROBE_IPK_STAGE}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" "${PROBE_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROBE_ROOT}/webos/icon.png" "${PROBE_ROOT}/webos/largeIcon.png" "${PROBE_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:wgpuprobe>" "${PROBE_IPK_STAGE}/bin/wgpuprobe"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${PROBE_IPK_STAGE}/bin/wgpuprobe"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROBE_SDL2_RUNTIME_LIBRARY}" "${PROBE_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${PROBE_IPK_STAGE}/lib/libSDL2-2.0.so.0"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROBE_DAWN_LIBRARY}" "${PROBE_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${PROBE_IPK_STAGE}/lib/libwebgpu_dawn.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROBE_ROOT}/LICENSE" "${PROBE_ROOT}/THIRD_PARTY_NOTICES.md" "${PROBE_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${DAWN_SOURCE_DIR}/LICENSE" "${PROBE_IPK_STAGE}/LICENSE-Dawn.txt"
    DEPENDS wgpuprobe
    COMMENT "Staging the IPK contents in ${PROBE_IPK_STAGE}"
    VERBATIM)

if(PROBE_ARES_PACKAGE)
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROBE_DIST_DIR}"
        COMMAND "${PROBE_ARES_PACKAGE}" --no-minify "${PROBE_IPK_STAGE}" -o "${PROBE_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}"
                -DIPK=${PROBE_DIST_DIR}/${PROBE_IPK_NAME}
                -DOUTPUT=${PROBE_DIST_DIR}/${PROBE_APP_ID}.manifest.json
                -DAPP_ID=${PROBE_APP_ID}
                -DVERSION=${PROJECT_VERSION}
                "-DTITLE=WebGPU webOS probe"
                "-DDESCRIPTION=Checks whether the TV can run a WebGPU (Dawn) game renderer such as the Luigi's Mansion port's."
                -DICON_URI=https://raw.githubusercontent.com/pustal/WebGPU-WebOS-probe/main/webos/largeIcon.png
                -DSOURCE_URL=https://github.com/pustal/WebGPU-WebOS-probe
                -P "${PROBE_ROOT}/cmake/WriteManifest.cmake"
        DEPENDS webos-stage
        COMMENT "Building ${PROBE_IPK_NAME}"
        VERBATIM)
else()
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E echo "ares-package not found: npm install -g @webos-tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E false
        DEPENDS webos-stage)
endif()
