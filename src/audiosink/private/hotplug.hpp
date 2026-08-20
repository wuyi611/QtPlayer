//
// hotplug.hpp - 音频输出设备热插拔监听器
//
// 基于 Qt Multimedia 的 QMediaDevices 监听系统音频输出设备的插拔事件,
// 变化时转发 audioOutputsChanged 信号, 由 PonyAudioSink 刷新设备列表并重启流。
// (设备枚举/打开本身走 PortAudio, 这里仅借用 Qt 的事件通知能力)
//
#pragma once

#include <QtCore>
#include <QMediaDevices>
#include <QAudioDevice>

class HotPlugDetector : public QObject {
    Q_OBJECT
private:
    QMediaDevices qMediaDevices; ///< Qt 多媒体设备监视器
public:
    /// 构造: 订阅系统音频输出设备变化事件
    HotPlugDetector(QObject *parent = nullptr) : QObject(parent)  {
        connect(&qMediaDevices, &QMediaDevices::audioOutputsChanged, this, &HotPlugDetector::audioOutputsChanged);
    }

signals:
    /// 音频输出设备集合发生变化(插拔/切换)
    void audioOutputsChanged();
};
