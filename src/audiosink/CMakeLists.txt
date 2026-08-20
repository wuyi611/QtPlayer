# audiosink 模块构建配置
# 基于 PortAudio 的音频输出库, 被 player 模块的 Playback 使用。
project(audiosink)

# 仅有的 .cpp 编译单元(其余均为头文件库)
set(CPP_SOURCES private/hotplug.hpp)
qt_add_library(${PROJECT_NAME} STATIC ${CPP_SOURCES})

# 导出本目录为 PUBLIC include, 使外部可 #include "audiosink.hpp" / "audioformat.hpp"
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_sources(${PROJECT_NAME} PRIVATE audiosink.hpp audioformat.hpp)

target_link_libraries(${PROJECT_NAME} PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Quick
    Qt::Multimedia      # 热插拔监听(QMediaDevices)
    portaudio_static    # 音频输出后端
    rwqueue             # moodycamel 无锁队列(dataInfoQueue)
    sonic               # 变速/变调/音量处理
    utils               # ponyplayer.h(线程常量/宏)
    ${FFmpeg}           # 采样格式枚举(libavutil)
)
