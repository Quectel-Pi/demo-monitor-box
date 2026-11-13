cmake_minimum_required(VERSION 3.16)

# Monitor Box: C++17 capture, inference and streaming backend
#
# 编译 (在 SDK 根目录):
#   source build.sh
#   buildapp projects/demo-monitor-box
#
# 或者直接:
#   cmake -S projects/demo-monitor-box -B build \
#         -DCMAKE_TOOLCHAIN_FILE=$CMAKE_TOOLCHAIN_FILE
#   cmake --build build
#
# 交叉工具链由 tools/cmake/aarch64-m2-debian-toolchain.cmake 提供, 那边已经把
# CMAKE_SYSROOT 指到 prebuilds/sysroot。
project(monitor_box LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Web UI 源文件 → 内嵌 C++ 头文件。页面不依赖前端框架，板端只需要 mb-capture,
# 因此构建时把 HTML/CSS/JS 编进二进制，避免额外部署静态目录。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(MB_WEB_UI_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/web/web_ui_generated.h")
    file(GLOB MB_WEB_FONT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/fonts/*.woff2")
    add_custom_target(monitor_box_web_ui
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_web_ui.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        BYPRODUCTS "${MB_WEB_UI_HEADER}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/app.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/i18n-en.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/app/mb_i18n.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/hicolor/128x128/apps/com.quectel.MonitorBox.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/fonts/LICENSE.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_web_ui.py"
            ${MB_WEB_FONT_FILES}
        COMMENT "Embedding Web UI")
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

# 这个 CMake 只用于**交叉编译** (宿主机 -> aarch64)。
# 板上没有 cmake, 原地编译走 tools/build_on_board.sh (直接调 g++)。
if(NOT CMAKE_SYSROOT)
    get_filename_component(_mb_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(CMAKE_SYSROOT "${_mb_root}/prebuilds/sysroot")
endif()
message(STATUS "monitor_box: CMAKE_SYSROOT = ${CMAKE_SYSROOT}")

# ---------------------------------------------------------------------------
# 库路径
# ---------------------------------------------------------------------------
# RKNN headers are not distributed with this repository. Resolve them from
# RKNN_INCLUDE_DIR, the Quectel SDK's rknpu2 external directory, or an
# optional local development checkout in third_party/rknn.
set(RKNN_INCLUDE_DIR "" CACHE PATH "Directory containing rknn_api.h")
if(NOT RKNN_INCLUDE_DIR AND DEFINED ENV{RKNN_INCLUDE_DIR})
    set(RKNN_INCLUDE_DIR "$ENV{RKNN_INCLUDE_DIR}")
endif()
if(NOT RKNN_INCLUDE_DIR)
    get_filename_component(_mb_sdk_parent "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    foreach(_mb_candidate
            "${_mb_sdk_parent}/RK3576_Linux6.1_BL01_v1.2.0/external/rknpu2/runtime/Linux/librknn_api/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn")
        if(EXISTS "${_mb_candidate}/rknn_api.h")
            set(RKNN_INCLUDE_DIR "${_mb_candidate}" CACHE PATH "Directory containing rknn_api.h" FORCE)
            break()
        endif()
    endforeach()
endif()
if(NOT RKNN_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 rknn_api.h；请设置 -DRKNN_INCLUDE_DIR=<包含 rknn_api.h 的目录>")
endif()
message(STATUS "monitor_box: rknn include = ${RKNN_INCLUDE_DIR}")

# librknnrt.so 只出现在 sysroot 的 /usr/lib 下 (不在 aarch64-linux-gnu 子目录),
# 所以单独找它。绝不能链 rknn_model_zoo/3rdparty 里那份 2.5.0: 模型是 2.3.2 转的。
find_library(RKNNRT_LIB NAMES rknnrt PATHS "${CMAKE_SYSROOT}/usr/lib" NO_DEFAULT_PATH)
if(NOT RKNNRT_LIB)
    message(FATAL_ERROR "找不到 librknnrt.so (期望在 ${CMAKE_SYSROOT}/usr/lib)")
endif()

find_library(RGA_LIB NAMES rga PATHS "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
             ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)
if(NOT RGA_LIB)
    message(FATAL_ERROR "找不到 librga.so (期望在 ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu)")
endif()

# H.264 硬件编码 (VEPU)。头文件在 sysroot 的 usr/include/rockchip/。
find_library(MPP_LIB NAMES rockchip_mpp PATHS "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
             ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)
if(NOT MPP_LIB)
    message(FATAL_ERROR "找不到 librockchip_mpp.so (期望在 ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu)")
endif()

find_package(Threads REQUIRED)

find_library(JPEG_LIB NAMES jpeg)
if(NOT JPEG_LIB)
    message(FATAL_ERROR "找不到 libjpeg")
endif()

message(STATUS "monitor_box: rknnrt = ${RKNNRT_LIB}")
message(STATUS "monitor_box: rga    = ${RGA_LIB}")
message(STATUS "monitor_box: jpeg   = ${JPEG_LIB}")
message(STATUS "monitor_box: mpp    = ${MPP_LIB}")

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
set(MB_SRCS
    src/common/geometry.cpp
    src/common/json.cpp
    src/event/dwell_guard.cpp
    src/event/event_engine.cpp
    src/event/speaker.cpp
    src/event/zone_alarm.cpp
    src/geo/geo_locator.cpp
    src/infer/clip/clip_encoder.cpp
    src/infer/clip/clip_tokenizer.cpp
    src/infer/infer_worker.cpp
    src/infer/jpeg_io.cpp
    src/infer/open_vocab.cpp
    src/infer/pose_post.cpp
    src/infer/pose_runner.cpp
    src/infer/preprocess.cpp
    src/infer/rknn_engine.cpp
    src/infer/yolo_post.cpp
    src/locate/ground_calib.cpp
    src/locate/ground_locator.cpp
    src/rules/activity_map.cpp
    src/rules/count_store.cpp
    src/rules/fall_detector.cpp
    src/rules/fire_detector.cpp
    src/rules/rule_bindings.cpp
    src/rules/rule_engine.cpp
    src/stream/h264_nal.cpp
    src/stream/mp4_writer.cpp
    src/stream/rtp_sender.cpp
    src/stream/video_pipeline.cpp
    src/track/tracker.cpp
    src/video/h264_encoder.cpp
    src/video/mpp_jpeg_decoder.cpp
    src/video/bev_osd.cpp
    src/video/region_overlay.cpp
    src/video/view_guard.cpp
    src/video/v4l2_source.cpp
    src/web/http_server.cpp
    src/web/web_api.cpp
)

# rknn_model_zoo 的画框工具, 只有 stdio/stdlib 依赖, 可以安全地拷进仓库。
# (image_utils.c 不行 —— 它要 turbojpeg.h / stb_image / im2d.h, sysroot 里没有 turbojpeg)
#
# 上游 image_drawing.c 用了 cos/sin 却没 include <math.h> (zoo 的编译单元里靠别处带进来),
# 用 -include 补上, 保持 vendored 文件与上游逐字节一致, 方便以后重新同步。
add_library(mb_zoo_draw STATIC third_party/zoo/image_drawing.c)
target_include_directories(mb_zoo_draw PUBLIC third_party/zoo)
target_compile_options(mb_zoo_draw PRIVATE -include math.h)
target_link_libraries(mb_zoo_draw PUBLIC m)

add_library(mb_core STATIC ${MB_SRCS})
if(TARGET monitor_box_web_ui)
    add_dependencies(mb_core monitor_box_web_ui)
endif()
target_include_directories(mb_core PUBLIC src ${RKNN_INCLUDE_DIR})
# bev_osd.cpp 在 OSD 上画鸟瞰图, 要用 zoo 的画线/写字
target_link_libraries(mb_core PUBLIC mb_zoo_draw ${RKNNRT_LIB} ${RGA_LIB} ${MPP_LIB} ${JPEG_LIB} Threads::Threads)

# ---------------------------------------------------------------------------
# 工具 (源码放 examples/, 和库 src/ 区分开)
#   mb-infer    单图推理与模型排错
#   mb-capture  摄像头 -> 检测 -> 跟踪 -> 规则 -> 事件 -> OSD -> H.264/UDP + Web
#   mb-selftest 合成画面验证 事件引擎 + H.264 + 告警录像 (不需要摄像头/模型)
#   mb-ruletest 规则引擎 + 跟踪器回归 (边界抖动/遮挡/ID 切换), 纯 CPU
# ---------------------------------------------------------------------------
add_executable(mb-infer examples/infer.cpp)
target_link_libraries(mb-infer PRIVATE mb_core mb_zoo_draw)

add_executable(mb-capture examples/capture.cpp)
target_link_libraries(mb-capture PRIVATE mb_core mb_zoo_draw)

add_executable(mb-selftest examples/selftest.cpp)
target_link_libraries(mb-selftest PRIVATE mb_core)

add_executable(mb-ruletest examples/ruletest.cpp)
target_link_libraries(mb-ruletest PRIVATE mb_core)

# ---------------------------------------------------------------------------
# 部署: cmake --install build --prefix /opt/monitor_box
#      (或 make install 走 overlay/)
# ---------------------------------------------------------------------------
install(TARGETS mb-infer mb-capture mb-selftest mb-ruletest RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/monitor_box FILES_MATCHING PATTERN "*.json")
install(FILES
    models/yolov8s-640-640.rknn
    models/yolov8n-640-640.rknn
    models/coco_80_labels_list.txt
    DESTINATION models)
