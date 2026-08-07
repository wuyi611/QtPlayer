#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QCoreApplication>
#include <QQmlContext>
#include "utils/include/logger.h"
#include "players.h"
#include "controller.h"
#include "playlist.h"
#include "wave/wave.hpp"
#include "hotloader.hpp"
#include "lyrics.h"
#include "crashreporter.hpp"
#include <csignal>


namespace PonyPlayer {

    // 保存程序启动时的完整命令行参数，崩溃重启时需要用到
    QStringList programArguments;

    /**
     * @brief 崩溃报告模式入口 — 显示错误报告窗口
     *
     * 调用路径:
     *   1. 用户手动: PonyPlayer.exe --crash-report --log-file xxx --message "..."
     *   2. 崩溃自动: signalHandler → startReporterProcess 启动新进程(带以上参数)
     *
     * @param argc 命令行参数个数
     * @param argv 命令行参数数组
     * @return QGuiApplication::exec() 的返回值
     */
    int reportErrorMain(int argc, char *argv[]) {
        // argv[0] = 程序自身路径
        QString program = argv[0];
        QString logFile;
        QString message;
        QStringList arguments;

        // 解析命令行参数: --log-file、--message、--crash-report 之前的原始参数
        for (int i = 1; i < argc;) {
            auto &&argument = argv[i];
            if (strcmp(argument, "--crash--report") == 0) {
                // 收集 --crash-report 之前的所有原始参数
                for (int j = 1; j < i; ++j) {
                    arguments.emplace_back(argument);
                }
            } else if (strcmp(argument, "--log-file") == 0 && i + 1 < argc) {
                logFile = argv[i + 1];
                i += 2;
            } else if (strcmp(argument, "--message") == 0 && i + 1 < argc) {
                message = argv[i + 1];
                i += 2;
            } else {
                i += 1;
            }
        }

        // 启动 QML 错误报告界面
        QGuiApplication app(argc, argv);
        const QUrl url(u"qrc:/view/IssueWindow.qml"_qs);
        QQmlApplicationEngine engine;
        CrashReporter crashReporter(message, logFile, program, arguments);
        engine.rootContext()->setContextProperty("crash_reporter", &crashReporter);
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                         &app, [url](QObject *obj, const QUrl &objUrl) {
                    if (!obj && url == objUrl)
                        QCoreApplication::exit(-1);
                }, Qt::QueuedConnection);
        engine.load(url);
        return QGuiApplication::exec();
    }

    /**
     * @brief 启动一个新的崩溃报告进程（本进程即将退出）
     *
     * 使用 QProcess::startDetached 启动自身可执行文件，
     * 并传入 --crash-report、--log-file、--message 参数。
     *
     * @param message 错误信息（如 "SIGNAL: 6"）
     */
    void startReporterProcess(const std::string &message) {
        QString &program = programArguments.front();
        QStringList arguments;

        // 复制原始启动参数
        for (int i = 1; i < programArguments.size(); ++i) {
            arguments.emplace_back(programArguments[i]);
        }

        // 附加崩溃报告相关参数
        arguments.append("--crash-report");
        arguments.append("--log-file");
        arguments.append(PonyPlayer::getLogFile());
        arguments.append("--message");
        arguments.append(message.c_str());

        // 启动独立进程（不等待，不阻塞）
        QProcess process(nullptr);
        process.setProgram(program);
        process.setArguments(arguments);
        qint64 pid;
        process.startDetached(&pid);
    }

    /**
     * @brief 信号处理器 — 捕获 SIGABRT / SIGSEGV
     *
     * 收到信号后启动崩溃报告进程，然后退出当前进程。
     * 注意: 信号处理函数中能做的操作非常有限，应尽快退出。
     */
    void signalHandler(int signum) {
        PonyPlayer::startReporterProcess("SIGNAL: " + std::to_string(signum));
        exit(signum);
    }

} // namespace PonyPlayer


/**
 * @brief 程序主入口
 *
 * 启动流程:
 *   1. 检查是否为崩溃报告模式 (--crash-report)，是则进入 reportErrorMain
 *   2. 保存完整命令行参数到 programArguments
 *   3. 注册信号处理器 (SIGABRT, SIGSEGV)
 *   4. 设置 OpenGL 渲染参数
 *   5. 注册所有 QML 类型
 *   6. 加载主界面 main.qml
 *   7. 进入 Qt 事件循环
 */
int main(int argc, char *argv[]) {

    // ---- 第一阶段: 启动模式判断 ----
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--crash-report") == 0) {
            // 命令行包含 --crash-report → 进入崩溃报告模式
            return PonyPlayer::reportErrorMain(argc, argv);
        }
        // 保存完整命令行参数（用于崩溃时重启自身）
        PonyPlayer::programArguments.append(argv[i]);
    }

    // ---- 第二阶段: 注册崩溃信号处理 ----
    signal(SIGABRT, PonyPlayer::signalHandler);   // abort() 触发
    signal(SIGSEGV, PonyPlayer::signalHandler);    // 段错误触发

    // ---- 第三阶段: 设置 OpenGL 渲染 ----
    // 指定 Qt Quick 场景图（Scene Graph）使用 OpenGL 作为底层渲染 API
    QQuickWindow::setGraphicsApi(QSGRendererInterface::GraphicsApi::OpenGL);
    QSurfaceFormat format;
    // 将 OpenGL 模式设置为 Core Profile（核心模式）
    format.setProfile(QSurfaceFormat::CoreProfile);
    // 请求系统创建 OpenGL 3.3 版本的上下文
    format.setVersion(3, 3);
    // 设置为全局默认格式
    QSurfaceFormat::setDefaultFormat(format);

    // ---- 第四阶段: 初始化 Qt 应用 ----
    QGuiApplication app(argc, argv);

    // ---- 第五阶段: 注册 C++ 类型到 QML ----
    registerPlayerQML();
    // 将 C++ 类注册为 QML 中的可实例化类型
    qmlRegisterType<WaveView>("WaveView", 1, 0, "WaveView");
    qmlRegisterType<Controller>("Controller", 1, 0, "Controller");
    qmlRegisterType<PlayList>("PlayList", 1, 0, "PlayList");
    qmlRegisterType<simpleListItem>("SimpleListItem", 1, 0, "SimpleListItem");
    qmlRegisterType<LyricsData>("LyricsData", 1, 0, "LyricsData");
    qmlRegisterType<LyricSentence>("LyricSentence", 1, 0, "LyricSentence");
    // 将指针类型 PlayListItem * 注册到 Qt 的元对象系统
    qRegisterMetaType<PlayListItem *>("PlayListItem");

    // 注册 PonyPlayer 命名空间（导出枚举到 QML）
    qmlRegisterUncreatableMetaObject(
            PonyPlayer::staticMetaObject,
            "ponyplayer.ns",
            1, 0,
            "PonyPlayerNS",
            "Error: only enums"
    );

    // 安装自定义日志处理器
    qInstallMessageHandler(PonyPlayer::logMessageHandler);

    // ---- 第六阶段: 加载 QML 主界面 ----
    const QUrl url(u"qrc:/view/main.qml"_qs);
    QQmlApplicationEngine engine;
    // 创建一个自定义的热加载器对象（监听本地qml文件变化，变化时重新加载qml界面
    HotLoader hotLoader(&engine);
    engine.rootContext()->setContextProperty("hotLoader", &hotLoader);

    // QML 加载失败时退出
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
                if (!obj && url == objUrl)
                    QCoreApplication::exit(-1);
            }, Qt::QueuedConnection);

    engine.load(url);

    // ---- 第七阶段: 进入事件循环 ----
    return QGuiApplication::exec();
}
