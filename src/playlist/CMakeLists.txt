# ============================================================
# playlist 模块构建脚本
# 本模块负责媒体库（播放列表）的核心功能：
#   - kv_engine.cpp    : 基于 SQLite 的键值存取引擎（PonyKVConnect / PonyKVList）
#   - playlist.cpp     : 播放列表后端数据操作（PlayList / PlayListItem）
#   - controller.cpp   : 前后端桥接控制器（Controller，供 QML 调用）
#   - info_accessor.cpp: 基于 FFmpeg 读取媒体文件元信息（infoAccessor）
# ============================================================

# 项目名称
project(playlist)

# 编译进静态库的源文件列表
set(CPP_SOURCES kv_engine.cpp playlist.cpp controller.cpp info_accessor.cpp)

# 将上述源文件编译为名为 playlist 的静态库（STATIC，链接进主程序而非单独分发）
qt_add_library(${PROJECT_NAME} STATIC ${CPP_SOURCES})

# 将本模块的头文件一并登记到目标中，便于 Qt/IDE 的依赖与自动生成(MOC)识别
target_sources(playlist PRIVATE
    include/kv_engine.h
    include/playlist.h
    include/controller.h
    include/info_accessor.h
)

# 对外公开 include 目录，使其他模块可以 #include 这些头文件
target_include_directories(${PROJECT_NAME} PUBLIC include)

# 链接依赖的 Qt 模块以及自定义 utils 库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Quick
    Qt::Sql
    utils
)

