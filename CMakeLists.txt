cmake_minimum_required(VERSION 3.24)

# 版本号唯一事实来源是 cpp/sys/defaults.hpp 的 VERSION：configure 时读出来喂给
# project()，exe 属性（zpin.rc）与关于页就自动同步，改一处即可。
file(READ "${CMAKE_SOURCE_DIR}/cpp/sys/defaults.hpp" ZPIN_DEFAULTS_SRC)
string(REGEX MATCH "VERSION[ \t]*=[ \t]*\"[0-9]+(\\.[0-9]+)*\"" ZPIN_VER_LINE "${ZPIN_DEFAULTS_SRC}")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" ZPIN_APP_VERSION "${ZPIN_VER_LINE}")
# 改了 defaults.hpp 里的版本号要自动重新 configure，否则 exe 属性还停在旧版本
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS cpp/sys/defaults.hpp)
project(ZPin VERSION ${ZPIN_APP_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()

if(MSVC)
    # 源码注释/文案是 UTF-8 中文；cl 默认按系统代码页(936)读会语法错乱
    add_compile_options(/utf-8 /W3)
endif()

# ---- Rust 核心：cargo 出 staticlib（桥接 C++ 已由 build.rs 编进库），
#      头文件由 build.rs 镜像到 target/<profile>/cxxbridge/include ----
# 全部构建产物（CMake 的 + cargo 的）都进同一个 build/ 目录，不额外再套一层：
#   - rust/ 目录里只留代码（src/ Cargo.toml Cargo.lock build.rs .cargo/）。
#     cargo 默认的 rust/target/ 实测能长到 3.5GB，和 src/ 混在一层会让源码目录
#     看起来莫名其妙巨大。
#   - 只留一个构建根目录，删掉 build/ 即等于全量清理（含 Rust 全量重编）。
#     （这一层早先叫 cmake/——名字和「CMake 脚本目录」的行业惯例撞车，脚本
#     目录没法用，2026-10 改名 build/，脚本回归 cmake/。）
# build/ 已被 .gitignore 忽略；cmake/ 现在是版本库里的脚本目录。
set(RUST_DIR ${CMAKE_SOURCE_DIR}/rust)
set(RUST_TARGET_DIR ${CMAKE_SOURCE_DIR}/build)
set(RUST_LIB ${RUST_TARGET_DIR}/release/zpin_core.lib)
set(RUST_INCLUDE ${RUST_TARGET_DIR}/release/cxxbridge/include)
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS ${RUST_DIR}/src/*.rs ${RUST_DIR}/build.rs)

add_custom_command(
    OUTPUT ${RUST_LIB}
    # CARGO_TARGET_DIR 显式传给 cargo（用 cmake -E env 注入），否则 cargo 会
    # 仍然写回默认的 rust/target/，上面挪目录就白改了
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
            cargo build --release
    WORKING_DIRECTORY ${RUST_DIR}
    # Cargo.toml / Cargo.lock 必须进 DEPENDS：只盯 .rs 的话，改了依赖版本或
    # cargo update 之后 ninja 会认为 ${RUST_LIB} 已是最新，根本不调用 cargo，
    # 直接把旧静态库链进 exe（「改了依赖但产物没变」的幽灵 bug 由此而来）
    DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml ${RUST_DIR}/Cargo.lock
    COMMENT "cargo build --release (zpin_core)"
    VERBATIM USES_TERMINAL
)
add_custom_target(rust_core ALL DEPENDS ${RUST_LIB})

# ---- C++ 界面层：五个领域子目录，新文件放进对应目录即可被 glob 到 ----
# app/   应用组装与共享业务服务（main/app/托盘/保存/历史/图标）
# sys/   系统交互与基础设施（配置/日志/Win32/Rust桥/热键/自启/更新/内存）
# capture/  截图会话（选区/吸附/标注/拼接/OCR）
# pin/   贴图窗口三件套
# prefs/ 设置对话框、帮助页与其专属控件
file(GLOB ZPIN_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/cpp/*/*.cpp)
set(ZPIN_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/cpp/app
    ${CMAKE_SOURCE_DIR}/cpp/sys
    ${CMAKE_SOURCE_DIR}/cpp/capture
    ${CMAKE_SOURCE_DIR}/cpp/pin
    ${CMAKE_SOURCE_DIR}/cpp/prefs)

qt_add_executable(zpin WIN32
    ${ZPIN_SOURCES}
)
add_dependencies(zpin rust_core)
# 源码里的 #include 统一是平文件名（"config.hpp"），不随目录改写——目录只是
# 归档方式，不是命名空间；把五个目录全部挂进 include 路径即可。
target_include_directories(zpin PRIVATE ${ZPIN_INCLUDE_DIRS} ${RUST_INCLUDE})

# ---- OpenCV（smart_erase 的内容感知填充；只用 core+imgproc，静态链接）----
# 一次性构建：tools\build_opencv.bat（源码与产物都在 build\opencv\，整个 build\
# 已 gitignore，所以既不进版本库，也守住「构建产物一律进 build\」这条规则）。
# 静态库名带版本后缀（opencv_core4100.lib），用通配匹配免去跟着版本改；链接顺序：
# imgproc 依赖 core，被依赖者放后面；ittnotify 是 core 自带的性能探针库，必须同链。
set(OPENCV_INSTALL ${CMAKE_SOURCE_DIR}/build/opencv/install)
file(GLOB OPENCV_CORE_LIB ${OPENCV_INSTALL}/staticlib/opencv_core*.lib)
file(GLOB OPENCV_IMGPROC_LIB ${OPENCV_INSTALL}/staticlib/opencv_imgproc*.lib)
if(NOT OPENCV_CORE_LIB OR NOT OPENCV_IMGPROC_LIB)
    message(FATAL_ERROR
        "找不到 OpenCV 静态库（${OPENCV_INSTALL}/staticlib/opencv_core*.lib）。\n"
        "按 README「构建准备」执行 tools\\build_opencv.bat 一次性构建。")
endif()
target_include_directories(zpin PRIVATE ${OPENCV_INSTALL}/include)
# zlib 在最后：core 的 FileStorage 引用 gz* 符号，供体库必须排在引用者之后
target_link_libraries(zpin PRIVATE ${OPENCV_IMGPROC_LIB} ${OPENCV_CORE_LIB}
                                   ${OPENCV_INSTALL}/staticlib/ittnotify.lib
                                   ${OPENCV_INSTALL}/staticlib/zlib.lib)
# directml：onnxruntime 静态包自带 DirectML 执行提供者（OCR 走 CPU 用不到它，
# 但符号必须解析）。运行期依赖 C:\Windows\System32\DirectML.dll（Win10 1903+ 自带）。
target_link_libraries(zpin PRIVATE Qt6::Widgets Qt6::Svg ${RUST_LIB}
                    user32 dwmapi ntdll directml winhttp)

# ---- genico：构建期把内嵌 SVG 渲染成 app_icon.ico（exe 资源用）----
qt_add_executable(genico
    tools/genico.cpp
    cpp/app/ui_app_icon.cpp
)
target_include_directories(genico PRIVATE ${ZPIN_INCLUDE_DIRS})
target_link_libraries(genico PRIVATE Qt6::Widgets Qt6::Svg)

# 图标产物落在构建目录：早先写进 assets/app_icon.ico（那是 git 跟踪的文件），
# 于是「只是编译一次」也会把源码目录改脏，git status 永远挂着一条 M。
# 仓库里那份 assets/app_icon.ico 留作基线资产，构建不再覆盖它。
set(ZPIN_ICO ${CMAKE_CURRENT_BINARY_DIR}/app_icon.ico)
get_filename_component(QT_BIN_DIR ${Qt6_DIR}/../../../bin ABSOLUTE)
add_custom_command(
    OUTPUT ${ZPIN_ICO}
    # genico 运行时要能加载 Qt6 DLL，注入 Qt bin 的 PATH
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${QT_BIN_DIR}\;$ENV{PATH}"
            $<TARGET_FILE:genico> ${ZPIN_ICO}
    DEPENDS genico
    COMMENT "genico: 渲染 app_icon.ico（构建目录）"
    VERBATIM
)
add_custom_target(app_icon_ico DEPENDS ${ZPIN_ICO})
add_dependencies(zpin app_icon_ico)

# ---- exe 图标与版本信息（.rc 生成到 build 目录，引用绝对路径的 ico）----
set(RC_IN ${CMAKE_SOURCE_DIR}/assets/zpin.rc)
set(RC_OUT ${CMAKE_CURRENT_BINARY_DIR}/zpin.rc)
# FILEVERSION 要的是 1,3,0,0 这种逗号形式，从 PROJECT_VERSION 展开而非手写
string(REPLACE "." "," ZPIN_VERSION_CSV "${PROJECT_VERSION}")
string(APPEND ZPIN_VERSION_CSV ",0")
# 走构建期自定义命令而非 configure_file：ninja -t clean 会删掉生成的 .rc，
# 只在 configure 时写过一次的话，清理后不重新 configure 就报 C1083 打不开源文件。
add_custom_command(
    OUTPUT ${RC_OUT}
    COMMAND ${CMAKE_COMMAND}
            -DIN=${RC_IN} -DOUT=${RC_OUT}
            -DVER=${PROJECT_VERSION} -DVERCSV=${ZPIN_VERSION_CSV} -DICO=${ZPIN_ICO}
            -P ${CMAKE_SOURCE_DIR}/cmake/genrc.cmake
    DEPENDS ${RC_IN} ${ZPIN_ICO}
    COMMENT "生成 zpin.rc（版本 ${PROJECT_VERSION}）"
    VERBATIM
)
add_custom_target(zpin_rc DEPENDS ${RC_OUT})
add_dependencies(zpin zpin_rc)
target_sources(zpin PRIVATE ${RC_OUT})
set_source_files_properties(${RC_OUT} PROPERTIES GENERATED TRUE)

