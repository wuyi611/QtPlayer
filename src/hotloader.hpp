//
// Created by ColorsWind on 2022/5/2.
//
// HotLoader — 开发期热重载支持
// 在 Debug 模式下，通过快捷键触发 QML 界面的热重载，无需重启应用即可看到修改效果。
//

#ifndef PONYPLAYER_HOTLOADER_H
#define PONYPLAYER_HOTLOADER_H

#include <QtCore>                   // Qt 核心功能（qDebug、Q_ASSERT 等）
#include <QQmlApplicationEngine>    // QML 应用引擎
#include <QQuickView>               // QQuickWindow（主窗口基类）
#include <filesystem>               // C++17 文件系统（用于构建 main.qml 路径）

/**
 * @brief 热重载管理器 — Debug 模式下提供 F5 重载 / F1/F2 崩溃测试
 *
 * 仅在 `QT_DEBUG` 宏定义时生效，Release 构建中所有操作为空。
 *
 * 使用方式：
 * - F5: 关闭当前窗口 → 清除组件缓存 → 重新加载 main.qml
 * - F1/F2: 抛出异常，测试程序的崩溃处理机制
 */
class HotLoader : public QObject {
    Q_OBJECT

private:
    /// QML 引擎指针（由外部传入，不拥有所有权）
    QQmlApplicationEngine *engine;

public:
    /// 构造函数：绑定到指定的 QML 引擎
    explicit HotLoader(QQmlApplicationEngine *e): engine(e) {
        qDebug() << "Construct HotLoader.";
    }

    /// F5 快捷键回调：热重载 QML 界面
    /// 流程：关闭主窗口 → 清除组件缓存 → 重新加载 main.qml
    Q_INVOKABLE void reload() {
#ifdef QT_DEBUG
        // 基于当前源文件路径定位 main.qml
        static std::string mainQML = std::filesystem::path(__FILE__).parent_path().string() + "/view/main.qml";
        auto *rootObject = engine->rootObjects().first();
        auto* mainWindow = qobject_cast<QQuickWindow*>(rootObject);
        Q_ASSERT( mainWindow != nullptr );
        mainWindow->close();                      // 关闭当前窗口
        engine->clearComponentCache();            // 清除 QML 缓存（否则修改不会生效）
        mainWindow->deleteLater();                // 延迟销毁旧窗口
        engine->load(QUrl::fromLocalFile(QString::fromUtf8(mainQML)));  // 重新加载
        qWarning() << "Complete hot reloading.";
#else
        Q_UNUSED(engine)   // Release 构建中抑制未使用变量警告
#endif
    }

    /// F1/F2 快捷键回调：主动崩溃，用于测试异常处理/崩溃恢复
    Q_INVOKABLE void crash() {
#ifdef QT_DEBUG
        throw std::runtime_error("Crash Test!");
#endif
    }
};

#endif //PONYPLAYER_HOTLOADER_H
