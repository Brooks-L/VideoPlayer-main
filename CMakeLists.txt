
cmake_minimum_required(VERSION 3.21)

project(VideoPlayer LANGUAGES CXX)

set(TARGET_NAME VideoPlayer)

# ============================================================
# 1. C++ / Qt configuration
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(MSVC)
    add_compile_options(/MP)
endif()

if(WIN32)
    add_compile_definitions(
        UNICODE
        _UNICODE
        _WINDOWS
        WIN32
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
    OpenGLWidgets
)

find_package(Threads REQUIRED)

# ============================================================
# 2. Header files
# ============================================================

set(HEADER_FILES
    src/mainwindow.h
    src/about.h
    src/app_settings.h
    src/audio_play_thread.h
    src/video_play_thread.h
    src/audio_decode_thread.h
    src/packets_sync.h
    src/video_decode_thread.h
    src/read_thread.h
    src/subtitle_decode_thread.h
    src/play_control_window.h
    src/start_play_thread.h
    src/version.h
    src/video_label.h
    src/video_state.h
    src/audio_effect_gl.h
    src/audio_effect_helper.h
    src/youtube_url_thread.h
    src/clickable_slider.h
    src/stopplay_waiting_thread.h
    src/playlist_window.h
    src/network_url_dlg.h
    src/common.h
    src/youtube_json.h
    src/ffmpeg_init.h
    src/imagecv_operations.h
    src/log.h
    src/player_skin.h
    src/qimage_convert_mat.h
    src/qimage_operation.h
)

# ============================================================
# 3. Source files
# ============================================================

set(SOURCE_FILES
    src/main.cpp
    src/about.cpp
    src/app_settings.cpp
    src/audio_decode_thread.cpp
    src/audio_play_thread.cpp
    src/ffmpeg_init.cpp
    src/imagecv_operations.cpp
    src/log.cpp
    src/mainwindow.cpp
    src/packets_sync.cpp
    src/player_skin.cpp
    src/play_control_window.cpp
    src/qimage_convert_mat.cpp
    src/qimage_operation.cpp
    src/read_thread.cpp
    src/start_play_thread.cpp
    src/subtitle_decode_thread.cpp
    src/video_decode_thread.cpp
    src/video_play_thread.cpp
    src/video_state.cpp
    src/youtube_url_dlg.cpp
    src/video_label.cpp
    src/audio_effect_gl.cpp
    src/audio_effect_helper.cpp
    src/youtube_url_thread.cpp
    src/clickable_slider.cpp
    src/stopplay_waiting_thread.cpp
    src/playlist_window.cpp
    src/network_url_dlg.cpp
    src/common.cpp
    src/youtube_json.cpp
)

# ============================================================
# 4. Qt UI / resource files
# ============================================================

list(APPEND SOURCE_FILES
    src/about.ui
    src/mainwindow.ui
    src/play_control_window.ui
    src/youtube_url_dlg.ui
    src/playlist_window.ui
    src/qmake_qmake_qm_files.qrc
    src/VideoPlayer_en_001.ts
)

# ============================================================
# 5. FFmpeg paths
# ============================================================

set(FFMPEG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ffmpeg"
)

set(FFMPEG_INCLUDE_DIR
    "${FFMPEG_ROOT}/include"
)

set(FFMPEG_LIB_DIR
    "${FFMPEG_ROOT}/lib"
)

set(FFMPEG_BIN_DIR
    "${FFMPEG_ROOT}/bin"
)

# FFmpeg import libraries
set(FFMPEG_LIB_NAMES
    avcodec
    avutil
    avformat
    swscale
    swresample
    avfilter
)

set(FFMPEG_LIBRARIES "")

foreach(LIB_NAME IN LISTS FFMPEG_LIB_NAMES)

    set(LIB_PATH
        "${FFMPEG_LIB_DIR}/${LIB_NAME}.lib"
    )

    if(NOT EXISTS "${LIB_PATH}")
        message(FATAL_ERROR
            "FFmpeg library not found: ${LIB_PATH}"
        )
    endif()

    list(APPEND FFMPEG_LIBRARIES
        "${LIB_PATH}"
    )

endforeach()

# ============================================================
# 6. OpenCV 4.9.0 paths
# ============================================================

set(OPENCV_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/opencv/build"
)

set(OPENCV_INCLUDE_DIR
    "${OPENCV_ROOT}/include"
)

set(OPENCV_LIB_DIR
    "${OPENCV_ROOT}/x64/vc16/lib"
)

set(OPENCV_BIN_DIR
    "${OPENCV_ROOT}/x64/vc16/bin"
)

# Release libraries
set(OPENCV_RELEASE_LIB
    "${OPENCV_LIB_DIR}/opencv_world490.lib"
)

set(OPENCV_RELEASE_DLL
    "${OPENCV_BIN_DIR}/opencv_world490.dll"
)

# Debug libraries
set(OPENCV_DEBUG_LIB
    "${OPENCV_LIB_DIR}/opencv_world490d.lib"
)

set(OPENCV_DEBUG_DLL
    "${OPENCV_BIN_DIR}/opencv_world490d.dll"
)

if(NOT EXISTS "${OPENCV_RELEASE_LIB}")
    message(FATAL_ERROR
        "OpenCV Release library not found: ${OPENCV_RELEASE_LIB}"
    )
endif()

if(NOT EXISTS "${OPENCV_RELEASE_DLL}")
    message(FATAL_ERROR
        "OpenCV Release DLL not found: ${OPENCV_RELEASE_DLL}"
    )
endif()

if(NOT EXISTS "${OPENCV_DEBUG_LIB}"
   OR NOT EXISTS "${OPENCV_DEBUG_DLL}")

    message(WARNING
        "OpenCV Debug library/DLL is missing. "
        "Debug builds require opencv_world490d.lib and "
        "opencv_world490d.dll. "
        "Release and RelWithDebInfo remain available."
    )

endif()

# ============================================================
# 7. Generate executable
# ============================================================

if(WIN32)
    add_executable(${TARGET_NAME} WIN32
        ${SOURCE_FILES}
        ${HEADER_FILES}
    )
else()
    add_executable(${TARGET_NAME}
        ${SOURCE_FILES}
        ${HEADER_FILES}
    )
endif()

# ============================================================
# 8. Include directories
# ============================================================

target_include_directories(${TARGET_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FFMPEG_INCLUDE_DIR}"
    "${OPENCV_INCLUDE_DIR}"
)

# ============================================================
# 9. Link libraries
# ============================================================

target_link_libraries(${TARGET_NAME} PRIVATE

    # Qt
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::OpenGLWidgets

    # Threads
    Threads::Threads

    # FFmpeg
    ${FFMPEG_LIBRARIES}

    # OpenCV: Debug / Release automatically selected
    debug     "${OPENCV_DEBUG_LIB}"
    optimized "${OPENCV_RELEASE_LIB}"
)

# ============================================================
# 10. Target properties
# ============================================================

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    VS_DEBUGGER_WORKING_DIRECTORY
        "$<TARGET_FILE_DIR:${TARGET_NAME}>"
)

install(TARGETS ${TARGET_NAME}
    RUNTIME DESTINATION bin
)

# ============================================================
# 11. Windows runtime DLL deployment
# ============================================================

if(WIN32)

    # --------------------------------------------------------
    # FFmpeg 7.x runtime DLLs
    # --------------------------------------------------------

    set(FFMPEG_REQUIRED_DLLS
        avcodec-61.dll
        avdevice-61.dll
        avfilter-10.dll
        avformat-61.dll
        avutil-59.dll
        postproc-58.dll
        swresample-5.dll
        swscale-8.dll
    )

    foreach(DLL_NAME IN LISTS FFMPEG_REQUIRED_DLLS)

        if(NOT EXISTS "${FFMPEG_BIN_DIR}/${DLL_NAME}")
            message(FATAL_ERROR
                "FFmpeg runtime DLL not found: "
                "${FFMPEG_BIN_DIR}/${DLL_NAME}. "
                "Please use a complete FFmpeg 7.x shared build."
            )
        endif()

    endforeach()

    # Copy all FFmpeg DLLs, including third-party dependencies.
    file(GLOB FFMPEG_RUNTIME_DLLS
        CONFIGURE_DEPENDS
        "${FFMPEG_BIN_DIR}/*.dll"
    )

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${TARGET_NAME}>"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Copying FFmpeg runtime DLLs"
    )

    # --------------------------------------------------------
    # OpenCV runtime DLL
    # --------------------------------------------------------

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${OPENCV_DEBUG_DLL},${OPENCV_RELEASE_DLL}>"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>"
        VERBATIM
        COMMENT "Copying matching OpenCV runtime DLL"
    )

    # --------------------------------------------------------
    # Copy tools directory
    # --------------------------------------------------------

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tools")

        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/src/tools"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/tools"
            VERBATIM
            COMMENT "Copying tools"
        )

    endif()

    # --------------------------------------------------------
    # Copy resource directory
    # --------------------------------------------------------

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/res")

        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/src/res"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/res"
            VERBATIM
            COMMENT "Copying resources"
        )

    endif()

    # --------------------------------------------------------
    # Qt runtime deployment
    # --------------------------------------------------------

    option(DEPLOY_QT_RUNTIME
        "Run windeployqt after building"
        ON
    )

    if(DEPLOY_QT_RUNTIME)

        get_filename_component(QT_INSTALL_ROOT
            "${Qt6_DIR}/../../.."
            ABSOLUTE
        )

        find_program(WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt.exe windeployqt
            HINTS
                "${QT_INSTALL_ROOT}/bin"
                "$ENV{QTDIR}/bin"
        )

        if(WINDEPLOYQT_EXECUTABLE)

            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    "$<IF:$<CONFIG:Debug>,--debug,--release>"
                    "$<TARGET_FILE:${TARGET_NAME}>"
                VERBATIM
                COMMENT "Deploying Qt runtime libraries"
            )

        else()

            message(WARNING
                "windeployqt was not found. "
                "Qt runtime DLLs will not be deployed automatically."
            )

        endif()

    endif()

endif()

# ============================================================
# 12. Build information
# ============================================================

message(STATUS "========================================")
message(STATUS "Project          : ${TARGET_NAME}")
message(STATUS "Qt version       : ${Qt6_VERSION}")
message(STATUS "FFmpeg directory : ${FFMPEG_ROOT}")
message(STATUS "OpenCV directory : ${OPENCV_ROOT}")
message(STATUS "C++ standard     : C++20")
message(STATUS "========================================")
