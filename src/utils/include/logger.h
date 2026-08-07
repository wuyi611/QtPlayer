//
// Created by ColorsWind on 2022/3/24.
//

#ifndef SOFTWAREENGINEERING_LOGGER_H
#define SOFTWAREENGINEERING_LOGGER_H
#include <QtCore>

/**
 * @brief PonyPlayer 项目命名空间
 *
 * 对外暴露两个日志相关函数:
 *   - logMessageHandler: Qt 消息处理器，注册到 qInstallMessageHandler
 *   - getLogFile: 获取当前日志文件路径（崩溃报告时使用）
 */
namespace PonyPlayer {
    /// Qt 消息处理回调 — 将所有 Qt 日志输出转发到 Logger 单例
    void logMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);
    /// 获取当前日志文件的绝对路径
    QString getLogFile();
}
#endif //SOFTWAREENGINEERING_LOGGER_H
