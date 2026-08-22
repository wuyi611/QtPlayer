//
// Created by ColorsWind on 2022/5/5.
//
// playback.hpp - Playback 类声明
// 负责音视频的最终输出与播放控制（音量、倍速、暂停/停止/同步等）。
//
#pragma once
#ifndef PONYPLAYER_VIDEOWORKER_H
#define PONYPLAYER_VIDEOWORKER_H

#include <QObject>
#include <QThread>
#include <QDebug>
#include <QCoreApplication>
#include <utility>
#include "demuxer.hpp"
#include "audiosink.hpp"
#include "frame.hpp"

/**
 * @brief 负责输出视频和音频(不含视频预览).
 *
 * 这个类负责将上层的帧输出到相应的设备. 这个类的RAII的. 如果没有特殊说明, 这个类的公有方法是线程安全的.
 *
 * 线程模型:
 * - 本对象被 moveToThread 到 m_affinityThread(即 PonyPlayer::PLAYBACK 线程), 所有内部状态
 *   (缓存帧、音频 sink 等)都只在该线程上访问;
 * - 外部线程通过"带 QPrivateSignal 的信号"把请求排队投递到该线程执行, 从而保证公有方法线程安全;
 * - m_isInterrupt + m_interruptCond 用于在暂停/停止时唤醒同步等待;
 * - m_workMutex 保证同一时刻只有一个播放循环(onWork)在运行.
 */
class Playback : public QObject {
Q_OBJECT
private:
    // 播放工作线程: 本对象被移动到该线程, 通过跨线程信号槽与外界通信
    QThread *m_affinityThread;
    // 解复用器(解码)指针, 由外部持有并传入, 本类不负责释放
    Demuxer *m_demuxer;
    // 缓存的视频帧: 用于"显示首帧"前预先取出一帧, 之后由 onWork 消费
    VideoFrameRef cacheVideoFrame;


    // 音频输出设备后端, 在播放线程启动时(QThread::started)创建
    PonyAudioSink *m_audioSink = nullptr;
    // 中断标志: pause()/stop() 置位, onWork 播放循环据此退出
    std::atomic<bool> m_isInterrupt;
    // 播放状态标志: true 表示播放循环正在运行
    std::atomic<bool> m_isPlaying;
    // 保护 m_isInterrupt 与 m_interruptCond 的互斥量
    std::mutex m_interruptMutex;
    // 保护播放循环的互斥量: pause()/stop() 阻塞等待 onWork 结束
    std::mutex m_workMutex;
    // 条件变量: 唤醒 syncTo() 中的睡眠等待(暂停/停止时立即醒来)
    std::condition_variable m_interruptCond;

    // playback 的速度, 不受倍速限制 (实际倍速由 m_audioSink 的 speed 控制)
    qreal m_speedFactor = 1.0;

    // 首选播放位置(秒), 供外部(如进度条/帧控制器)查询的推荐显示位置
    std::atomic<qreal> m_preferablePos = 0.0;

    /**
     * 切换播放状态并发出 stateChanged 信号.
     * @param isPlaying 新的播放状态
     */
    inline void changeState(bool isPlaying) {
        m_isPlaying = isPlaying;
        emit stateChanged(isPlaying);
    }

    /**
     * 音视频同步: 根据当前播放进度计算需要睡眠的时间, 使视频帧与音频时钟对齐.
     *
     * 规则:
     * - 纯音频: 按固定 1/30 秒帧间隔推进, 位置取自音频已处理时长;
     * - 有视频时: 用"队列头视频帧时间戳与音频已播放时间"的差值作为睡眠时长;
     * - 音频被禁用(block)或倍速超过 MAX_SPEED_FACTOR 时, 以视频时间戳为基准推进;
     * - 睡眠期间若 m_isInterrupt 被置位, 立即醒来(暂停/停止时避免长时间阻塞).
     * @param current 当前视频帧的 PTS(秒)
     */
    inline void syncTo(qreal current) {
        bool backward = m_demuxer->isBackward();
        double duration;
        if (!m_demuxer->hasVideo()) {
            // 纯音频: 按固定帧间隔(1/30 秒)推进, 位置取自音频已处理时长
            duration = 1. / 30;
            m_preferablePos = m_audioSink->getProcessSecs(backward);
        } else {
            qreal pos = m_demuxer->frontPicture();
            if (isnan(pos)) { return; }
            m_preferablePos = current;
            if (m_audioSink->isBlock()) {
                // 由于没有音频(音频被禁用), 以视频时间戳为基准同步
                duration = (current - pos) / m_audioSink->speed();
            } else {
                if (m_audioSink->speed() > 2 - 1e-5) {
                    // 高倍速(>2x)时音频跟不上视频, 需要主动跳过落后的视频帧
                    if (!backward) {
                        m_demuxer->skipPicture([this, backward](qreal framePos) {
                            return framePos < m_audioSink->getProcessSecs(backward);
                        });
                    } else {
                        m_demuxer->skipPicture([this, backward](qreal framePos) {
                            return framePos > m_audioSink->getProcessSecs(backward);
                        });
                    }
                }
                duration = m_demuxer->frontPicture() - m_audioSink->getProcessSecs(backward);
            }
            if (backward) { duration = -duration; }
        }
        if (duration > 0) {
            if (duration > 1) {
                qWarning() << "Sleep long duration" << duration << "s";
            }
            std::unique_lock lock(m_interruptMutex);
            if (!m_isInterrupt) {
                // 等待 duration 秒, 期间可被 pause()/stop() 通过条件变量唤醒
                m_interruptCond.wait_for(lock, std::chrono::duration<double>(duration));
            }
        } else {
            qWarning() << "Sleep negative duration" << duration << "s";
        }
    }

    /**
     * 从解复用器批量取出音频样本并写入音频输出.
     * @param batch 本次最多写入的音频样本数
     * @return 是否成功; 返回 false 表示音频数据已取尽(播放到结尾)
     */
    inline bool writeAudio(int batch) {
        if (m_audioSink->isBlock()) { return true; }
        for (int i = 0; i < batch && m_audioSink->freeByte() > MAX_AUDIO_FRAME_SIZE; ++i) {
            AudioFrame sample = m_demuxer->getSample();
            if (!sample.isValid()) { return false; }
            m_audioSink->write(reinterpret_cast<const char *>(sample.getSampleData()), sample.getDataLen());
        }
        return true;
    }

    // 以下成员函数要求在 PLAYBACK 线程上调用
    PONY_GUARD_BY(PLAYBACK)

    /**
     * 获取一帧视频: 优先取缓存帧(showFirstVideoFrame 预取的), 否则从解复用器取.
     * @return 视频帧引用; 无效表示视频已播放完毕
     */
    VideoFrameRef getVideoFrame() {
        if (cacheVideoFrame.isValid()) {
            VideoFrameRef ret = std::move(cacheVideoFrame);
            cacheVideoFrame = {};
            return ret;
        } else {
            return m_demuxer->getPicture();
        }
    }

public:
    /**
     * 构造函数.
     * 创建播放线程, 将本对象移动到该线程, 并建立所有跨线程信号槽连接.
     * @param demuxer 解复用器指针(外部持有生命周期), 用于取音视频帧
     * @param parent  Qt 父对象(当前实现未使用, 统一挂在空父对象下)
     */
    Playback(Demuxer *demuxer, QObject *parent) : QObject(nullptr), m_demuxer(demuxer) {
        m_affinityThread = new QThread;
        m_affinityThread->setObjectName(PonyPlayer::PLAYBACK);
        this->moveToThread(m_affinityThread);
        // 播放控制信号 → 播放线程上的处理函数(队列连接, 自动跨线程)
        connect(this, &Playback::startWork, this, &Playback::onWork);
        connect(this, &Playback::stopWork, this, [this] { this->m_audioSink->stop(); });
        connect(this, &Playback::setAudioStartPoint, this, [this](qreal t) { this->m_audioSink->setStartPoint(t); });
        connect(this, &Playback::setAudioVolume, this, [this](qreal volume) { this->m_audioSink->setVolume(volume); });
        connect(this, &Playback::setAudioPitch, this, [this](qreal pitch) { this->m_audioSink->setPitch(pitch); });
        // 已读
        connect(this, &Playback::setAudioSpeed, this, [this](qreal speed) {
            m_speedFactor = speed;
            this->m_audioSink->setSpeed(speed);
            if (speed > PonyAudioSink::MAX_SPEED_FACTOR) {
                if (this->m_audioSink->isBlock()) { return; }
                // 需要禁用音频(倍速过高, 音频无法跟上), 并请求重同步
                this->m_audioSink->setBlockState(true);
                emit requestResynchronization(false, false); // queue connection
            } else if (speed <= PonyAudioSink::MAX_SPEED_FACTOR) {
                if (!this->m_audioSink->isBlock()) { return; }
                // 需要重新启动音频(倍速恢复到可支持范围), 并请求重同步
                this->m_audioSink->setBlockState(false);
                emit requestResynchronization(true, false); // queue connection
            }
        });
        // 显示首帧: 若尚无缓存帧则预取一帧并输出给渲染器
        connect(this, &Playback::showFirstVideoFrame, this, [this] {
            if (!cacheVideoFrame.isValid()) { cacheVideoFrame = m_demuxer->getPicture(); }
            emit setPicture(cacheVideoFrame);
        });
        // 首帧输出后清空缓存(该帧已交给渲染器)
        connect(this, &Playback::showFirstVideoFrame, this, [this] {
            cacheVideoFrame = {};
        });
        // 清空音频环形缓冲区
        connect(this, &Playback::clearRingBuffer, this, [this] { this->m_audioSink->clear(); });
        // 播放线程启动时: 创建音频输出设备并建立其信号转发
        connect(m_affinityThread, &QThread::started, [this] {
            // 在 Playback 线程上初始化
            this->m_audioSink = new PonyAudioSink(PonyPlayer::DEFAULT_AUDIO_FORMAT);
            // 设备切换: 通知外部并请求音视频重同步
            connect(m_audioSink, &PonyAudioSink::signalDeviceSwitched, this, [this] {
                emit signalDeviceSwitched();
                emit requestResynchronization(!this->m_audioSink->isBlock(), true);
            }, Qt::QueuedConnection);
            // 外部选择输出设备的请求 → 转发给音频后端
            connect(this, &Playback::signalSetSelectedAudioOutputDevice, m_audioSink,
                    &PonyAudioSink::requestDeviceSwitch);
            // 设备列表变化 → 转发给外部
            connect(m_audioSink, &PonyAudioSink::signalAudioOutputDeviceListChanged, this, [this] {
                emit signalAudioOutputDevicesListChanged();
            });
            emit signalAudioOutputDevicesListChanged();
        });
        m_affinityThread->start();
    }

    /**
     * 获取首选播放位置(秒), 线程安全.
     * 由播放循环在同步时不断更新, 供进度条等外部组件查询.
     */
    PONY_THREAD_SAFE qreal getPreferablePos() {
        return m_preferablePos;
    }

    /**
     * 获取当前音频输出设备的格式.
     * @return 当前设备格式
     */
    PonyAudioFormat getDeviceFormat() {
        std::unique_lock lock(m_workMutex);
        return m_audioSink->getCurrentDeviceFormat();
    }

    /**
     * 获取音频已播放的位置(秒).
     * @param backward 是否处于倒放模式
     * @return 音频位置; 倍速超过 MAX_SPEED_FACTOR 时音频被禁用, 抛出 ILLEGAL_STATE
     */
    [[nodiscard]] qreal getAudioPos(bool backward) const {
        if (m_speedFactor < PonyAudioSink::MAX_SPEED_FACTOR) {
            return m_audioSink->getProcessSecs(backward);
        } else {
            ILLEGAL_STATE("AudioPos not available.");
        }
    }

    /**
     * 设置期望的音频输出格式(解码前配置).
     * @param format 目标音频格式
     */
    void setDesiredFormat(const PonyAudioFormat& format) {
        m_audioSink->setFormat(format);
    }

//    qreal getVideoPos() const {
//        return m_preferablePos.load();
//    }

    /**
     * 析构函数: 退出播放线程. (RAII: 对象销毁即停止播放)
     */
    virtual ~Playback() {
        m_affinityThread->quit();
    }

    /**
     * 设置音量, 线程安全, 异步生效.
     * @param volume 音量(0.0 ~ 1.0)
     */
    void setVolume(qreal volume) {
        emit setAudioVolume(volume, QPrivateSignal());
    }

    /**
     * 设置音调(变速不变调的比例), 线程安全, 异步生效.
     * @param pitch 音调因子
     */
    void setPitch(qreal pitch) {
        emit setAudioPitch(pitch, QPrivateSignal());
    }

    /**
     * 设置播放倍速, 线程安全, 异步生效.
     * 超过 MAX_SPEED_FACTOR 时音频会被禁用(仅按视频同步).
     * @param speed 倍速
     */
    void setSpeed(qreal speed) {
        emit setAudioSpeed(speed, QPrivateSignal());
    }

    /**
     * 选择音频输出设备, 线程安全, 异步生效.
     * @param deviceName 设备名称
     */
    void setSelectedAudioOutputDevice(QString deviceName) {
        emit signalSetSelectedAudioOutputDevice(std::move(deviceName));
    }

    /**
     * 获取当前选中的音频输出设备名称.
     * @return 设备名; 音频后端未就绪时返回空串
     */
    QString getSelectedAudioOutputDevice() {
        return m_audioSink ? m_audioSink->getSelectedOutputDevice() : "";
    }

    /**
     * 显示第一帧画面: 预取一帧视频并输出到渲染器, 用于播放开始前的画面预览.
     */
    void showFrame() {
        emit showFirstVideoFrame(QPrivateSignal());
    }

    /**
     * 清空缓存的视频帧.
     */
    void clearCacheFrame() {
        //emit clearCacheVideoFrame(QPrivateSignal());
    }

    /**
     * 是否正在播放
     * @return 状态
     */
    bool isPlaying() { return m_isPlaying; }

    /**
     * 是否请求停止
     * @return 状态
     */
    bool isInterrupted() { return m_isInterrupt; }

    /**
     * 设置音频起始时间点(秒), 并清除中断标志.
     * 用于 seek 后重新从指定位置开始播放.
     * @param startPoint 起始时间(秒)
     */
    void setStartPoint(qreal startPoint) {
        m_isInterrupt = false;
//        m_preferablePos = startPoint;
        qDebug() << "SetStartPoint" << startPoint;
        emit setAudioStartPoint(startPoint, QPrivateSignal());
    }

    /**
     * 开始进行处理, 发送信号后方法将立即返回.
     * 实际的播放循环在播放线程的 onWork 槽中执行.
     */
    void start() {
//        std::unique_lock lock(m_workMutex);
        m_isInterrupt = false;
        emit startWork(QPrivateSignal());
    }

    /**
     * 清空内部缓冲区, 需要保证此刻没有读写操作.
     */
    void clear() {
        emit clearRingBuffer(QPrivateSignal());
    }

    /**
     * 尽快暂停音视频播放, 这个方法将会阻塞直到当前工作停止. 这个方法不会丢失数据.
     * 实现: 置中断标志并唤醒同步睡眠, 然后阻塞等待播放循环退出(获取 m_workMutex).
     */
    void pause() {
        std::unique_lock cond_lock(m_interruptMutex);
        // ① 通知 worker：该停了
        m_isInterrupt = true;
        // ② 如果 syncTo 正在 sleep，唤醒它
        m_interruptCond.notify_all();
        cond_lock.unlock();
        // ③ 这里阻塞，等 onWork 放锁
        std::unique_lock lock(m_workMutex);
    }

    /**
     * 立即停止, 清空缓冲区的数据.
     * 在 pause() 的基础上, 额外停止音频输出、归零起始时间戳并清空音频缓冲.
     */
    void stop() {
        // 第一步：中断播放循环，等它退出（和 pause 完全相同）
        std::unique_lock cond_lock(m_interruptMutex);
        m_isInterrupt = true;
        m_interruptCond.notify_all();
        cond_lock.unlock();
        // 阻塞到 onWork() 释放锁
        std::unique_lock lock(m_workMutex); // make sure stop

        // 第二步：彻底清理（pause 没有的部分）
        // 停掉音频输出设备（m_audioSink->stop()），关闭音频流
        emit stopWork(QPrivateSignal());
        // 把音频起始时间戳归零
        emit setAudioStartPoint(0.0, QPrivateSignal());
        // 清空环形缓冲区里残留的音频帧
        emit clearRingBuffer(QPrivateSignal());
    }

    /**
     * 获取可用的音频输出设备列表.
     * @return 设备名列表; 音频后端未就绪时为空列表
     */
    QStringList getAudioDeviceList() { return m_audioSink ? m_audioSink->getAudioDeviceList() : QStringList(); };

    /**
     * 获取当前音调因子.
     * @return 音调因子; 音频后端未就绪时为 1.0
     */
    qreal getPitch() { return m_audioSink ? m_audioSink->pitch() : 1.0; }


private slots:

    /**
     * 播放音视频. 需要保证 demuxer 可以正常阻塞.
     *
     * 播放主循环: 取视频帧→输出给渲染器→批量写音频→处理事件→同步睡眠, 直到:
     * - 被中断(pause/stop 置位 m_isInterrupt);
     * - 视频或音频数据取尽(发出 resourcesEnd).
     * 通过 m_workMutex 保证同一时刻只有一个循环实例在运行(未抢到锁则直接返回).
     */
    void onWork() {
        std::unique_lock lock(m_workMutex, std::defer_lock);
        if (!lock.try_lock()) { return; } // not allow neat run
        changeState(true);
        writeAudio(5);
        m_audioSink->start();
        while (!m_isInterrupt) {
            VideoFrameRef pic = getVideoFrame();
            if (!pic.isValid()) {
                // 视频取尽: 等待音频播完并通知资源结束
                m_audioSink->waitComplete();
                emit resourcesEnd();
                break;
            }
//            m_videoPos = pic.getPTS();
            emit setPicture(pic);
            if (!writeAudio(static_cast<int>(10 * m_audioSink->speed()))) {
                // 音频取尽: 等待音频播完并通知资源结束
                m_audioSink->waitComplete();
                emit resourcesEnd();
                break;
            }
            QCoreApplication::processEvents(); // process setVolume setSpeed etc
            syncTo(pic.getPTS());
        }
        m_audioSink->pause();
        changeState(false);
        lock.unlock();
    };



signals:
    // 以下带 QPrivateSignal 的信号无法被外部直接 emit, 只能通过公有方法间接触发

    /** 请求开始播放(在播放线程上触发 onWork 槽) */
    void startWork(QPrivateSignal);

    /** 请求停止音频输出 */
    void stopWork(QPrivateSignal);

    /** 请求清空音频环形缓冲区 */
    void clearRingBuffer(QPrivateSignal);

    /** 设置音频起始时间点(秒) */
    void setAudioStartPoint(qreal startPoint, QPrivateSignal);

    /** 设置音量 */
    void setAudioVolume(qreal volume, QPrivateSignal);

    /** 设置音调 */
    void setAudioPitch(qreal pitch, QPrivateSignal);

    /** 设置播放倍速 */
    void setAudioSpeed(qreal speed, QPrivateSignal);

    /** 选择音频输出设备(转发给音频后端) */
    void signalSetSelectedAudioOutputDevice(QString);

    /** 音频输出设备已切换 */
    void signalDeviceSwitched();

    /** 请求显示第一帧视频(内部使用, 由 showFrame() 触发) */
    void showFirstVideoFrame(QPrivateSignal);

    /** 请求清空缓存视频帧 */
    void clearCacheVideoFrame(QPrivateSignal);

    /** 输出一帧视频给渲染器 */
    void setPicture(VideoFrameRef pic);

    /** 播放状态变化(true=开始播放, false=停止) */
    void stateChanged(bool isPlaying);

    /** 音视频资源(数据)已播放完毕 */
    void resourcesEnd();

    /** 音频输出设备列表发生变化 */
    void signalAudioOutputDevicesListChanged();

    /**
     * 由于设备切换, 音频倍速调整等原因需要下层重新同步
     * @param enableAudio 是否启用音频
     * @param updateAudioFormat 是否更新音频格式
     */
    void requestResynchronization(bool enableAudio, bool updateAudioFormat);


};

#endif //PONYPLAYER_VIDEOWORKER_H
