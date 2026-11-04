cmake_minimum_required(VERSION 3.16)

# 版本号只在这一个地方改。
#
# 数字的含义（语义化版本 MAJOR.MINOR.PATCH）：
#   MAJOR  出现不兼容的改动（配置格式变了、需要用户重新设置）
#   MINOR  加了新功能，旧配置还能用
#   PATCH  只修 bug，行为不变
#
# 1.0.0 的门槛：**经过真实课堂验证**。在那之前一直留在 0.x。
# 版本历史见 CHANGELOG.md。
project(BoardMark VERSION 0.15.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 不需要 OpenCV，也不需要任何第三方图像库：
# Qt 6 的 QImage + Windows 的 BitBlt 已经够用。
# Network 用来调视觉模型 —— 用的是 OpenAI 兼容的 /chat/completions，
# 通义千问、智谱、豆包、OpenAI、本地 Ollama 都能直接接。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

add_executable(BoardMark
    src/main.cpp

    src/app/tray_app.cpp
    src/app/settings.cpp
    src/app/autostart.cpp
    src/app/theme.cpp
    src/app/theme.qrc

    src/capture/window_detector.cpp
    src/capture/screen_capture.cpp
    src/capture/mouse_hook.cpp
    src/capture/capture_engine.cpp
    src/capture/whiteboard.cpp

    src/analysis/frame_analyzer.cpp
    src/analysis/keyframe_rules.cpp

    src/ai/classifier.cpp
    src/ai/archiver.cpp
    src/ai/organizer.cpp
    src/ai/note_organizer.cpp
    src/ai/usage.cpp

    src/storage/session.cpp
)

target_include_directories(BoardMark PRIVATE src)

# 把版本号编进程序里，这样 --info 和「关于」对话框里能看到，
# 老师反馈问题时能直接说出自己是哪个版本 —— 没有这个，
# "我这边有点问题"这种反馈根本没法定位。
target_compile_definitions(BoardMark PRIVATE
    CM_VERSION="${PROJECT_VERSION}"
    CM_BUILD_DATE="${CMAKE_BUILD_TYPE} build"
)

target_link_libraries(BoardMark PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    user32
    gdi32
    psapi
)

# 静态链接 GCC 运行时（libgcc / libstdc++）。
#
# **不这么做的话，程序拷到别的电脑上根本跑不起来。**
# 默认情况下 exe 会依赖 libgcc_s_seh-1.dll 和 libstdc++-6.dll，
# 而这两个只存在于开发机的 MinGW 目录里 —— 别人的电脑上没有，
# 报错是「由于找不到 libgcc_s_seh-1.dll，无法继续执行代码」。
# 开发机上因为 PATH 里有 MinGW 的 bin 目录，所以完全看不出来这个问题。
#
# 静态链接后 exe 会大一点，但少两个外部依赖，值得。
if(MINGW)
    # -static 还必须带上：只写 -static-libgcc -static-libstdc++ 的话，
    # libstdc++ 会直接依赖 libwinpthread-1.dll，等于把一个依赖换成另一个。
    # 加上 -static 之后这三个运行时 DLL 全部进 exe，一个都不剩。
    # （Qt 的 DLL 不受影响 —— 它们是导入库，本来就要动态加载。）
    target_link_options(BoardMark PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# 控制台子系统：--info / --grab / --classify 诊断模式能看到输出。
# 托盘模式启动时会把控制台窗口隐藏掉（见 main.cpp 的 hideOwnConsole），
# 所以老师看不到黑框，而命令行诊断照常可用。
