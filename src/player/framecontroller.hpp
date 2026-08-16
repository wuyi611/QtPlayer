//
// Created by ColorsWind on 2022/5/5.
//

#pragma once

#include <QObject>
#include <utility>
#include "playback.hpp"

/**
 * @class FrameController
 * @brief 帧控制器 — 解码层（Demuxer）与输出层（Playback）之间的桥梁
 *
 * FrameController 是 PonyPlayer 播放架构中的中间协调层，运行在独立的 FRAME 线程上。
 * 它负责:
 *   - 管理 Demuxer（解码）和 Playback（音视频输出）的生命周期
 *   - 处理上层（UI/Playlist）发来的播放控制命令（打开、播放、暂停、停止、进度跳转等）
 *   - 协调音轨切换、播放方向切换、音频设备切换等复杂操作
 *   - 通过 Qt 信号槽机制跨线程转发控制指令
 *
 * 线程模型:
 *   - 构造函数创建名为 "FRAME" 的独立 QThread
 *   - 通过 moveToThread() 将自身所有槽函数绑定到 FRAME 线程执行
 *   - Demuxer 运行在 DECODER 线程，Playback 运行在 PLAYBACK 线程
 *   - 三个线程通过 Qt::QueuedConnection / Qt::BlockingQueuedConnection 协同工作
 *
 * 典型调用流程:
 *   openFile() → start() → (pause/seek/backward/forward) → close() / stop()
 *
 * 生命周期:
 *   播放列表打开时创建，程序退出或播放列表清空时销毁.
 *
 * @see Demuxer 解码调度器
 * @see Playback 音视频输出
 */
class FrameController : public QObject {
Q_OBJECT
private:
    /// FRAME 线程指针（FrameController 自身所在的线程）
    QThread *m_affinityThread = nullptr;
    /// 解封装器指针（运行在 DECODER 线程）
    Demuxer *m_demuxer = nullptr;
    /// 音视频输出控制器指针（运行在 PLAYBACK 线程）
    Playback *m_playback = nullptr;

public:
    /**
     * @brief 构造函数 — 创建 FRAME 线程并将自身移动过去
     * @param parent 父 QObject（未使用，始终传 nullptr）
     *
     * 创建名为 "FRAME" 的 QThread，通过 moveToThread 将 FrameController
     * 的所有槽函数绑定到该线程执行。连接线程的 started 信号到 initOnThread()
     * 以在该线程上初始化 Demuxer 和 Playback。线程创建后立即启动。
     */
    explicit FrameController([[maybe_unused]] QObject *parent) : QObject(nullptr) {
        m_affinityThread = new QThread;
        m_affinityThread->setObjectName(PonyPlayer::FRAME);
        this->moveToThread(m_affinityThread);
        connect(m_affinityThread, &QThread::started, this, &FrameController::initOnThread);
        m_affinityThread->start();
    }

private slots:

    /**
     * @brief 在 FRAME 线程上初始化 Demuxer 和 Playback
     *
     * 此槽函数在 m_affinityThread 启动时被调用（Qt::AutoConnection 默认为直连，
     * 因为 FrameController 已 moveToThread 到 m_affinityThread）。
     *
     * 初始化步骤:
     *   1. 创建 Demuxer（DECODER 线程）和 Playback（PLAYBACK 线程）
     *   2. 建立信号槽连接，协调三个线程之间的数据流和控制流
     *
     * 关键信号槽连接说明:
     *   - Playback::setPicture → FrameController::setPicture:
     *     将解码后的视频帧传递给上层 UI 显示 (DirectConnection)
     *   - Playback::stateChanged → FrameController::playbackStateChanged:
     *     转发播放状态变化 (DirectConnection)
     *   - FrameController::signalDecoderOpenFile → Demuxer::openFile:
     *     转发文件打开请求到解码线程
     *   - FrameController::signalDecoderSeek → Demuxer::seek:
     *     BLOCKING 连接 — 必须等待解码线程完成 seek 操作后才能继续
     *   - Demuxer::openFileResult → lambda:
     *     文件打开成功后设置音频格式、启动解码和输出
     *   - Playback::requestResynchronization → lambda:
     *     设备切换或倍速超出范围时重新同步音视频 (QueuedConnection)
     */
    void initOnThread() {
        this->m_demuxer = new Demuxer{this};
        this->m_playback = new Playback{m_demuxer, this};
        connect(m_playback, &Playback::setPicture, this, &FrameController::setPicture, Qt::DirectConnection);
        connect(m_playback, &Playback::stateChanged, this, &FrameController::playbackStateChanged,
                Qt::DirectConnection);
        // 已读
        connect(this, &FrameController::signalDecoderOpenFile, m_demuxer, &Demuxer::openFile);
        // WARNING: BLOCKING_QUEUED_CONNECTION — 必须等待 seek 完成!
        connect(this, &FrameController::signalDecoderSeek, m_demuxer, &Demuxer::seek, Qt::BlockingQueuedConnection);
        // 文件打开完成后的初始化: 设置音频格式 → 启动解码器 → 清空缓冲 → 显示首帧
        connect(m_demuxer, &Demuxer::openFileResult, this, [this](PonyPlayer::OpenFileResultType result) {
            if (result != PonyPlayer::OpenFileResultType::FAILED) {
                m_playback->setDesiredFormat(m_demuxer->getInputFormat());
                m_demuxer->setOutputFormat(m_playback->getDeviceFormat());
                m_demuxer->start();
                m_playback->clearCacheFrame();
                m_playback->showFrame();
            }
            emit openFileResult(result);
        });
        connect(m_playback, &Playback::resourcesEnd, this, &FrameController::resourcesEnd, Qt::DirectConnection);
        // 音轨切换: 保存当前进度 → 暂停 → 切换 → 恢复
        connect(this, &FrameController::signalDecoderSetTrack, m_demuxer, &Demuxer::setTrack);
        connect(this, &FrameController::signalSetTrack, this, [this](int i) {
            qreal pos = m_playback->getPreferablePos();
            m_playback->stop();
            m_demuxer->pause();
            emit signalDecoderSetTrack(i);
            m_playback->setDesiredFormat(m_demuxer->getInputFormat());
            m_demuxer->setOutputFormat(m_playback->getDeviceFormat());
            seek(pos);
        });
        // 切换到倒放: 保存进度 → 暂停 → 切换方向 → seek → 恢复播放
        connect(this, &FrameController::signalBackward, this, [this] {
            qreal pos = m_playback->getPreferablePos();
            m_playback->stop();
            m_demuxer->pause();
            m_demuxer->backward();
            seek(pos);
            m_demuxer->start();
        });
        // 切换到正放: 保存进度 → 暂停 → 切换方向 → seek → 恢复播放
        connect(this, &FrameController::signalForward, this, [this] {
            qreal pos = m_playback->getPreferablePos();
            m_playback->stop();
            m_demuxer->pause();
            m_demuxer->forward();
            seek(pos);
            m_demuxer->start();
        });
        // 音频设备切换或倍速调整后需要重新同步
        // enableAudio: 是否启用音频（倍速超过最大值时禁用）
        // updateAudioFormat: 是否需要更新音频输出格式
        connect(m_playback, &Playback::requestResynchronization, this, [this](bool enableAudio, bool updateAudioFormat) {
            if (!m_demuxer->isFileOpen()) return;
            bool isPlay = m_playback->isPlaying();
            qreal pos = m_playback->getPreferablePos();
            m_playback->stop();
            m_demuxer->pause();
            if (updateAudioFormat) {
                m_playback->setDesiredFormat(m_demuxer->getInputFormat());
                m_demuxer->setOutputFormat(m_playback->getDeviceFormat());
            }
            m_demuxer->setEnableAudio(enableAudio);
            seek(pos);
            m_demuxer->start();
            if (isPlay) { m_playback->start(); }
        }, Qt::QueuedConnection);
        connect(m_playback, &Playback::signalAudioOutputDevicesListChanged, this,
                &FrameController::signalAudioOutputDevicesChanged);
        connect(m_playback, &Playback::signalDeviceSwitched, this, [this] {
            emit signalDeviceSwitched();
        });
    }

public:

    /**
     * @brief 析构函数 — 退出 FRAME 线程
     *
     * 调用 QThread::quit() 请求线程退出。
     * 注意: 不负责清理 Demuxer 和 Playback，它们作为 QObject 子对象会被 Qt 自动销毁。
     */
    ~FrameController() override {
        m_affinityThread->quit();
    }

    /**
     * @brief 切换音轨
     * @param i 目标音轨索引
     *
     * 通过信号槽异步执行: 保存当前进度 → 停止输出 → 暂停解码 → 切换音轨 → seek 恢复
     */
    void setTrack(int i) {
        emit signalSetTrack(i);
    }

    /**
     * @brief 设置音频输出设备
     * @param deviceName 目标设备名称
     */
    void setSelectedAudioOutputDevice(QString deviceName) {
        m_playback->setSelectedAudioOutputDevice(std::move(deviceName));
    }

    /**
     * @brief 获取当前选中的音频输出设备名称
     * @return 设备名称字符串
     */
    QString getSelectedAudioOutputDevice() { return m_playback->getSelectedAudioOutputDevice(); }

    /**
     * @brief 切换到倒放模式
     *
     * 线程安全（PONY_THREAD_SAFE），通过信号槽异步执行切换操作。
     */
    PONY_THREAD_SAFE void backward() {
        emit signalBackward();
    }

    /**
     * @brief 切换到正放模式
     *
     * 线程安全（PONY_THREAD_SAFE），通过信号槽异步执行切换操作。
     */
    PONY_THREAD_SAFE void forward() {
        emit signalForward();
    }

    /**
     * @brief 获取当前播放进度（参考位置）
     * @return 进度值（单位：秒）
     *
     * 线程安全（PONY_THREAD_SAFE）。返回 Playback 中记录的首选播放位置，
     * 该值由音频输出或视频同步逻辑实时更新。
     */
    PONY_THREAD_SAFE qreal getPreferablePos() {
        return m_playback->getPreferablePos();
    }

    /**
     * @brief 获取音频总时长
     * @return 音频时长（单位：秒）
     *
     * 线程安全。直接从 Demuxer 获取，无需加锁。
     */
    qreal getAudioDuration() { return m_demuxer->audioDuration(); }

    /**
     * @brief 获取视频总时长
     * @return 视频时长（单位：秒）
     *
     * 线程安全。直接从 Demuxer 获取，无需加锁。
     */
    qreal getVideoDuration() { return m_demuxer->videoDuration(); }

    /**
     * @brief 获取音轨列表
     * @return 音轨名称列表，无文件打开时返回 {"没有打开的文件"}
     *
     * 线程安全。直接从 Demuxer 获取。
     */
    QStringList getTracks() { return m_demuxer->getTracks(); }

    /**
     * @brief 获取当前音调（pitch）值
     * @return 音调倍率，默认 1.0
     */
    qreal getPitch() { return m_playback ? m_playback->getPitch() : 1.0; }

    /**
     * @brief 查询当前文件是否包含视频流
     * @return true 表示包含视频轨道
     */
    bool hasVideo() { return m_demuxer->hasVideo(); }

    /**
     * @brief 设置音量
     * @param volume 音量值（0.0 ~ 1.0）
     */
    void setVolume(qreal volume) { m_playback->setVolume(volume); }

    /**
     * @brief 设置音调
     * @param pitch 音调倍率
     */
    void setPitch(qreal pitch) { m_playback->setPitch(pitch); }

    /**
     * @brief 设置播放速度
     * @param speed 速度倍率（1.0 为正常速度）
     */
    void setSpeed(qreal speed) { m_playback->setSpeed(speed); }

    /**
     * @brief 获取可用的音频输出设备列表
     * @return 设备名称列表，Playback 未初始化时返回空列表
     */
    QStringList getAudioDeviceList() { return m_playback ? m_playback->getAudioDeviceList() : QStringList(); }

public slots:

    /**
     * @brief 打开媒体文件
     * @param path 本地文件路径
     *
     * 通过信号 signalDecoderOpenFile 将打开请求转发到 DECODER 线程的 Demuxer。
     * Demuxer 完成打开后会通过 openFileResult 信号通知结果。
     */
    void openFile(const QString &path) {
        qDebug() << "Open file" << path;
        emit signalDecoderOpenFile(path.toStdString());
    }


    /**
     * @brief 暂停播放
     *
     * 调用 Playback::pause() 暂停音视频输出。
     * 注意: 仅暂停 Playback，不解码器继续运行。
     */
    void pause() {
        qDebug() << "Pausing";
        m_playback->pause();
    }

    /**
     * @brief 停止播放并关闭文件
     *
     * 依次调用:
     *   1. Demuxer::flush() — 清空解码缓冲队列
     *   2. Demuxer::close() — 销毁正放/倒放解码调度器
     *   3. Playback::stop() — 停止音视频输出并清空输出缓冲
     */
    void stop() {
        qDebug() << "Stopping";
        m_demuxer->flush();
        m_demuxer->close();
        m_playback->stop();
    }

    /**
     * @brief 关闭当前文件（不解码器 flush）
     *
     * 与 stop() 的区别: 不调用 flush()，适用于快速关闭场景。
     */
    void close() {
        qDebug() << "Closing";
        m_demuxer->close();
        m_playback->stop();
    }

    /**
     * @brief 开始/恢复播放
     *
     * 依次启动解码器和音视频输出:
     *   1. Demuxer::start() — 恢复解码器线程运行
     *   2. Playback::start() — 开始音视频输出
     */
    void start() {
        qDebug() << "Starting";
        m_demuxer->start();
        m_playback->start();
    }

    /**
     * @brief 跳转播放进度
     * @param seekPos 目标位置（单位：秒）
     *
     * 完整的进度跳转流程:
     *   1. Playback::stop()           — 停止音视频输出
     *   2. Demuxer::pause()           — 暂停解码器（阻塞等待，保证后续帧请求被阻塞）
     *   3. signalDecoderSeek(seekPos) — 通过 BLOCKING 连接通知解码器跳转（等待完成）
     *   4. Demuxer::flush()           — 清空解码缓冲中的旧帧
     *   5. Demuxer::start()           — 恢复解码器运行
     *   6. 根据播放方向确定起始点:
     *      - 倒放: 取当前视频帧 PTS 作为起始点（dispatcher 保证帧已就绪）
     *      - 正放: 跳过所有 PTS < seekPos 的视频帧和音频帧
     *   7. signalPositionChangedBySeek() — 通知上层进度已变化
     *   8. Playback::setStartPoint()  — 设置音频输出起始点
     *   9. Playback::showFrame()      — 显示当前帧
     *
     * WARNING: 必须保证 seek 前后所有状态（尤其是 PTS）已正确更新，
     * 否则 video 线程可能会长时间阻塞。
     */
    void seek(qreal seekPos) {
        qDebug() << "Start seek for" << seekPos;
        m_playback->stop();
        m_demuxer->pause();  // blocking — 保证后续 pic/sample 请求可被阻塞

        // WARNING: 必须保证所有状态（尤其是 PTS）已正确更新，
        // 否则 video 线程将长时间阻塞。
        emit signalDecoderSeek(seekPos); // blocking connection
        m_demuxer->flush();
        m_demuxer->start();

        bool backward = m_demuxer->isBackward();
        // 根据音频帧 PTS 可能更精确，但倒放时不可用
        qreal startPoint;
        if (backward) {
            // 倒放时不需要跳帧（dispatcher 已保证帧就绪）
            if (m_demuxer->hasVideo()) {
                startPoint = m_demuxer->frontPicture();
            } else {
                startPoint = seekPos;
            }
        } else {
            // 正放: 跳过位置之前的视频帧
            if (m_demuxer->hasVideo()) {
                m_demuxer->skipPicture([seekPos](qreal framePos) { return framePos < seekPos; });
            }
            // 跳过位置之前的音频帧，同时记录起始帧 PTS
            m_demuxer->skipSample(
                    [seekPos, &startPoint](qreal framePos) { return startPoint = framePos, framePos < seekPos; });

        }

        emit signalPositionChangedBySeek(); // 通知上层更新进度显示
        m_playback->setStartPoint(startPoint);
        m_playback->showFrame();

        qDebug() << "End seek for" << seekPos;
    }

signals:

    /**
     * @brief 通知解码器打开文件
     * @param path 文件路径（UTF-8）
     *
     * 跨线程信号，由 DECODER 线程上的 Demuxer::openFile 槽处理。
     */
    void signalDecoderOpenFile(std::string path);

    /**
     * @brief 通知解码器跳转进度
     * @param pos 目标位置（秒）
     *
     * 使用 Qt::BlockingQueuedConnection，调用线程会阻塞直到解码器 seek 完成。
     */
    void signalDecoderSeek(qreal pos);

    /**
     * @brief 通知上层 UI 进度已通过 seek 改变
     *
     * 由 seek() 在 seek 完成后发射，用于更新进度条等 UI 元素。
     */
    void signalPositionChangedBySeek();

    /**
     * @brief 请求切换音轨（内部信号，转发到 signalDecoderSetTrack）
     * @param i 音轨索引
     */
    void signalSetTrack(int i);

    /**
     * @brief 通知解码器切换音轨
     * @param i 音轨索引
     */
    void signalDecoderSetTrack(int i);

    /**
     * @brief 请求切换到倒放模式
     */
    void signalBackward();

    /**
     * @brief 请求切换到正放模式
     */
    void signalForward();

    /**
     * @brief 通知上层音频输出设备列表已变化
     */
    void signalAudioOutputDevicesChanged();

    /**
     * @brief 通知上层音频输出设备已切换
     */
    void signalDeviceSwitched();

    /**
     * @brief 文件打开结果信号
     * @param result 打开结果（SUCCESS / FAILED）
     *
     * 由 Demuxer::openFileResult 经 FrameController 转发给上层 UI。
     */
    void openFileResult(PonyPlayer::OpenFileResultType result);

    /**
     * @brief 播放状态变化信号
     * @param isPlaying true 表示正在播放，false 表示已暂停/停止
     */
    void playbackStateChanged(bool isPlaying);

    /**
     * @brief 资源播放完毕信号
     *
     * 当 Playback 检测到音视频数据已全部输出后发射。
     */
    void resourcesEnd();

    /**
     * @brief 视频帧更新信号
     * @param pic 当前需要显示的视频帧
     *
     * 由 Playback 发射，FrameController 转发给上层 UI 进行渲染。
     */
    void setPicture(VideoFrameRef pic);


};
