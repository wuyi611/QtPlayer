//
// Created by ColorsWind on 2022/3/24.
//
#include <QDateTime>
#include <QMutex>
#include <QFileInfo>
#include <iostream>
#include <random>

#include "logger.h"
#include "ponyplayer.h"

/**
 * @brief 日志记录器（单例，类定义隐藏在 .cpp 中）
 *
 * 设计要点:
 *   - 类定义不暴露到头文件，外部无法直接实例化
 *   - 通过 getLoggerInstance() 获取唯一实例 (Meyers 单例)
 *   - 对外 API: PonyPlayer::logMessageHandler / PonyPlayer::getLogFile
 *   - 双输出: stderr (控制台) + 日志文件
 *   - 线程安全: 所有写操作通过 QMutex 保护
 *   - 自动清理: 构造时删除 7 天前的旧日志
 */
class Logger {
private:
    QTextStream qTextStream{stderr};      // 控制台输出流
    QTextStream logStream;                // 文件输出流
    QMutex mutex;                         // 保证多线程写入安全
    QFile logFile;                        // 日志文件句柄

    /// 日志级别 → 单字母缩写 (Debug→D, Warning→W, Info→I, Fatal→F, Critical→C)
    const QMap<QtMsgType, QString> types = {
            {QtDebugMsg, "D"},
            {QtWarningMsg, "W"},
            {QtInfoMsg, "I"},
            {QtFatalMsg, "F"},
            {QtCriticalMsg, "C"}
    };

    /// 获取当前日期时间戳，格式: [yyyy-MM-dd hh:mm:ss]
    static QString getCurrentDateTime() {
        return QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss]");
    }

public:
    /// 生成指定长度的随机大写字母数字串（用于日志文件名防冲突）
    static std::string randStr(int length) {
        char tmp;
        std::string buffer;

        // 硬件/系统熵源随机种子
        std::random_device rd;
        // 随机数引擎
        std::default_random_engine random(rd());

        for (int i = 0; i < length; i++) {
            // 循环生成length个随机字符
            tmp = static_cast<char>(random() % 36);
            if (tmp < 10) {
                tmp += '0';       // 0-9
            } else {
                tmp -= 10;
                tmp += 'A';       // A-Z
            }
            buffer += tmp;
        }
        return buffer;
    }

    /// 构造函数 — 创建日志文件并清理过期日志 (7 天)
    Logger() {
        // 用户主目录
        auto home = PonyPlayer::getHome();
        QDir dir(home);
        dir.mkdir("log");
        dir.cd("log");

        // 计算 7 天前的日期，用于清理旧日志
        auto ddl = QDateTime::currentDateTime().addDays(-7).toString("yyyy-MM-dd");

        for (auto &filename : dir.entryList({"*.log"})) {
            auto birthTime = filename.mid(0, 10);       // 文件名前 10 位是日期
            if (birthTime < ddl) {
                dir.remove(filename);                    // 删除过期日志
            }
        }

        // 创建新日志文件: 日期-16位随机串.log
        QString logFilename = home + QString("/log/") + QDateTime::currentDateTime().toString("yyyy-MM-dd-")
                              + randStr(16).c_str() + ".log";
        logFile.setFileName(logFilename);
        logFile.open(QIODevice::ReadWrite | QIODevice::Text);
        logStream.setDevice(&logFile);
    }

    ~Logger() {
        logFile.close();
    }

    /// 获取当前日志文件的绝对路径
    QString getLogFile() {
        return QFileInfo(logFile).absoluteFilePath();
    }

#ifdef QT_DEBUG
    /**
     * @brief Debug 模式日志输出 — 包含函数名/类名信息
     *
     * 输出格式:
     *   [时间戳] 级别 类名::函数名 消息
     *   例如: [2026-08-06 23:08:28] D PlayList::PlayList PlayList init!
     *
     * 函数名解析逻辑:
     *   - "retType ClassName::funcName(params)" → "ClassName::funcName"
     *   - "retType funcName(params)"            → "funcName"
     *   - "ClassName::ClassName(params)"        → "ClassName" (构造/析构)
     */
    inline void log(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
        QMutexLocker locker(&mutex);
        auto currentDateTime = getCurrentDateTime();

        // 写入时间戳和日志级别
        qTextStream << currentDateTime << " " << types.constFind(type).value();
        logStream << currentDateTime << " " << types.constFind(type).value();

        QString function = context.function;       // Qt 提供的完整函数签名
        qsizetype colon = function.indexOf(':');   // 找 :: (类成员函数标志)
        qsizetype retType = function.indexOf(' '); // 找第一个空格 (返回值类型结尾)

        if (function.isEmpty()) {
            // 无函数信息，跳过
        } else if (colon >= 0) {
            // 类成员函数: "int ClassName::funcName(params)"
            qsizetype param = function.indexOf('(', colon);
            if (retType < 0 || retType > colon) {
                // 构造/析构函数 (无返回值类型): "ClassName::ClassName(params)"
                qTextStream << " " << function.mid(colon + 2, param - (colon + 2));
                logStream << " " << function.mid(colon + 2, param - (colon + 2));
            } else {
                // 普通成员函数: "int ClassName::funcName(params)"
                QString clazz = function.mid(retType + 1, (colon - (retType + 1)));
                QString func = function.mid(colon + 2, param - (colon + 2));
                qTextStream << " " << clazz << "::" << func;
                logStream << " " << clazz << "::" << func;
            }
        } else {
            // 全局函数: "int funcName(params)"
            qsizetype param = function.indexOf('(');
            qTextStream << " " << function.mid(retType + 1, param - (retType + 1));
            logStream << " " << function.mid(retType + 1, param - (retType + 1));
        }

        // 写入消息体并刷新
        qTextStream << " " << msg << "\n";
        qTextStream.flush();
        logStream << " " << msg << "\n";
        logStream.flush();
    }
#else
    /**
     * @brief Release 模式日志输出 — 精简格式，不含函数名
     *
     * 输出格式: [时间戳] 级别 消息
     */
    inline void log(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
        QMutexLocker locker(&mutex);
        auto currentDateTime = getCurrentDateTime();
        qTextStream << currentDateTime << " " << types.constFind(type).value() << " " << msg << "\n";
        qTextStream.flush();
        logStream << currentDateTime << " " << types.constFind(type).value() << " " << msg << "\n";
        logStream.flush();
    }
#endif

};


/**
 * @brief 获取 Logger 的唯一实例 (Meyers 单例)
 *
 * C++11 保证: 函数内 static 局部变量的初始化是线程安全的，
 * 且只在第一次调用时执行一次。
 */
static Logger* getLoggerInstance() {
    static Logger logger;
    return &logger;
}

// ---- 对外 API ----

/// Qt 消息处理回调 — 注册到 qInstallMessageHandler
void PonyPlayer::logMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    getLoggerInstance()->log(type, context, msg);
}

/// 获取日志文件路径 — 供崩溃报告进程使用
QString PonyPlayer::getLogFile() {
    return getLoggerInstance()->getLogFile();
}
