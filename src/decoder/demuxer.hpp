/**
 * @file demuxer.hpp
 * @brief FFmpeg 解封装与解码调度器 — PonyPlayer 解码层的核心协调者
 *
 * Demuxer 封装了正放（DecodeDispatcher）和倒放（ReverseDecodeDispatcher）两套解码管线，
 * 对外提供统一的帧/采样获取、进度跳转、播放方向切换等接口。
 *
 * 线程模型:
 *   - 构造后 moveToThread(DECODER线程)，所有公开方法均在 DECODER 线程执行
 *   - m_workerLock 保护 m_worker / m_forward / m_backward 指针的并发访问
 *
 * 生命周期:
 *   伴随整个程序运行，在 FrameController 析构时销毁。
 *
 * @see DecodeDispatcher 正向解码调度器
 * @see ReverseDecodeDispatcher 反向解码调度器
 */

//
// Created by ColorsWind on 2022/5/6.
//
#pragma once

#include <QObject>
#include <utility>
#include "private/dispatcher.hpp"
#include "audioformat.hpp"

/**
 * @class Demuxer
 * @brief 解封装器 — 管理正放/倒放两条解码管线，提供音视频帧获取与进度控制
 *
 * 设计要点:
 *   - 内部持有 DecodeDispatcher（正放）和 ReverseDecodeDispatcher（倒放），通过 m_worker 指针切换
 *   - 所有公开接口通过 m_workerLock 互斥锁保证线程安全
 *   - 使用 PONY_THREAD_AFFINITY(DECODER) 声明线程亲和性
 *   - 构造函数创建独立 DECODER 线程并将自身移动过去
 *
 * 典型调用流程:
 *   openFile() → start() → getPicture()/getSample() 循环 → pause() → close()
 *   进度跳转: pause() → seek() → flush() → start()
 *   方向切换: pause() → backward()/forward() → start()
 *
 * 生命周期伴随整个程序运行.
 */
class Demuxer : public QObject {
    Q_OBJECT
    PONY_THREAD_AFFINITY(DECODER)
private:
    /// 当前活跃的解码调度器指针（指向 m_forward 或 m_backward）
    DemuxDispatcherBase *m_worker = nullptr;
    /// 正向解码调度器（负责正常播放方向的解码）
    DecodeDispatcher *m_forward = nullptr;
    /// 反向解码调度器（负责倒放方向的解码）
    ReverseDecodeDispatcher *m_backward = nullptr;

    /// DECODER 线程指针（Demuxer 自身所在的线程）
    QThread *m_affinityThread = nullptr;
    /// 保护 m_worker / m_forward / m_backward 的互斥锁
    std::mutex m_workerLock;
public:


    /**
     * @brief 构造函数 — 创建独立解码线程并将自身移动过去
     * @param parent 父 QObject（实际未使用，始终传 nullptr）
     *
     * 创建名为 "DECODER" 的 QThread，通过 moveToThread 将 Demuxer
     * 的所有槽函数绑定到该线程执行。线程创建后立即启动。
     */
    explicit Demuxer(QObject *parent) : QObject(nullptr) {
        m_affinityThread = new QThread;
        m_affinityThread->setObjectName(PonyPlayer::DECODER);
        this->moveToThread(m_affinityThread);
        m_affinityThread->start();
    }

    /// 析构 — 退出解码线程（不负责清理 dispatcher，由 close() 完成）
    ~Demuxer() override {
        qDebug() << "Destroy Demuxer";
        m_affinityThread->quit();
    }

    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 获取当前视频帧（出队并移除）
     * @return 视频帧引用，队列空时返回无效帧
     */
    VideoFrameRef getPicture() {
        std::unique_lock lock(m_workerLock);
        return m_worker->getPicture();
    }

    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 查看队首视频帧的时间戳（不出队）
     * @return 队首帧的 pts（单位：秒），队列空时返回负值
     */
    qreal frontPicture() {
        std::unique_lock lock(m_workerLock);
        return m_worker->frontPicture();
    }

    /**
     * @brief 跳过队列中满足条件的视频帧
     * @param predicate 判断函数，参数为帧 pts（秒），返回 true 则跳过
     * @return 实际跳过的帧数
     */
    PONY_THREAD_SAFE int skipPicture(const std::function<bool(qreal)> &predicate) {
        std::unique_lock lock(m_workerLock);
        return m_worker->skipPicture(predicate);
    }

    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 获取当前音频采样帧（出队并移除）
     * @return 音频帧，队列空时返回无效帧
     */
    AudioFrame getSample() {
        std::unique_lock lock(m_workerLock);
        return m_worker->getSample();
    }


    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 查看队首音频帧的时间戳（不出队）
     * @return 队首音频帧的 pts（单位：秒），队列空时返回负值
     */
    qreal frontSample() {
        std::unique_lock lock(m_workerLock);
        return m_worker->frontSample();
    }

    /**
     * @brief 跳过队列中满足条件的音频帧
     * @param predicate 判断函数，参数为帧 pts（秒），返回 true 则跳过
     * @return 实际跳过的帧数
     */
    PONY_THREAD_SAFE int skipSample(const std::function<bool(qreal)> &predicate) {
        std::unique_lock lock(m_workerLock);
        return m_worker->skipSample(predicate);
    }


    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 获取音频总时长
     * @return 音频时长（单位：秒），无文件打开时返回 0.0
     */
    qreal audioDuration() {
        std::unique_lock lock(m_workerLock);
        return m_forward ? m_forward->getAudionLength() : 0.0;
    }

    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 获取视频总时长
     * @return 视频时长（单位：秒），无文件打开时返回 0.0
     */
    qreal videoDuration() {
        std::unique_lock lock(m_workerLock);
        return m_forward ? m_forward->getVideoLength() : 0.0;
    }

    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    /**
     * @brief 获取音轨列表
     * @return 音轨名称列表，无文件打开时返回 {"没有打开的文件"}
     */
    QStringList getTracks() {
        std::unique_lock lock(m_workerLock);
        if (m_forward) {
            return m_forward->getTracks();
        } else {
            return {u"没有打开的文件"_qs};
        }
    }

    /**
     * @brief 查询当前播放方向是否为倒放
     * @return true 表示当前处于倒放模式
     *
     * 通过 dynamic_cast 判断 m_worker 是否指向 ReverseDecodeDispatcher。
     *
     * 当前是否倒放
     * @return
     */
    PONY_THREAD_SAFE bool isBackward() {
        std::unique_lock lock(m_workerLock);
        return dynamic_cast<ReverseDecodeDispatcher *>(m_worker);
    }

    /**
     * @brief 查询当前文件是否包含视频流
     * @return true 表示包含视频轨道
     */
    PONY_THREAD_SAFE bool hasVideo() {
        std::unique_lock lock(m_workerLock);
        return m_forward && m_forward->hasVideo();
    }



    /**
     * @brief 暂停解码器线程 — 向 DecodeThread 发送暂停信号并唤醒阻塞等待的线程
     *
     * 此方法不阻塞，调用后解码器线程会在当前帧处理完毕后进入暂停状态。
     * 通常作为 seek / backward / forward 等操作的前置步骤。
     *
     * @see DecodeDispatcher::statePause
     *
     * 向 DecodeThread 发送信号尽快暂停解码, 并唤醒阻塞在上面的线程.
     * @see DecodeDispatcher::statePause
    */
    PONY_CONDITION("OpenFileResult")
    PONY_THREAD_SAFE void pause() {
        std::unique_lock lock(m_workerLock);
        m_worker->statePause();
    }

    /**
     * @brief 查询当前是否有文件打开
     * @return true 表示已打开文件（m_worker 非空）
     */
    PONY_THREAD_SAFE bool isFileOpen() {
        std::unique_lock lock(m_workerLock);
        return m_worker != nullptr;
    }

    /**
     * @brief 清空解码缓冲区中的旧帧 — 阻塞直到队列中所有旧帧清理完成
     *
     * 通常在 seek 之后调用，确保后续 getPicture/getSample 拿到的是新位置的帧。
     * 必须在 FRAME 线程调用。
     *
     * @see DecodeDispatcher::flush
     *
     * 清空旧的帧, 这个方法会阻塞直到队列中的所有旧帧清理完成.
     * @see DecodeDispatcher::flush
     */
    PONY_GUARD_BY(FRAME) void flush() {
        std::unique_lock lock(m_workerLock);
        m_worker->flush();
    }

    /**
     * @brief 启动解码器 — 在 DECODER 线程恢复解码工作
     *
     * 非阻塞调用，但保证返回后新的帧请求能够被阻塞（等待解码完成）。
     *
     * 在 DecodeThread 启动解码器, 这个方法是非阻塞的, 但是可以保证返回后队里请求能够被阻塞.
     */
    PONY_THREAD_SAFE void start() {
        std::unique_lock lock(m_workerLock);
        qDebug() << "Start Decoder";
        m_worker->stateResume();
    }

    /**
     * @brief 启用/禁用音频解码
     * @param enable true 启用音频，false 禁用（仅解码视频）
     */
    void setEnableAudio(bool enable) {
        m_worker->setEnableAudio(enable);
    }

    /**
     * @brief 获取音频输入格式（源文件的采样率/通道数等）
     * @return 音频格式描述，无文件打开时返回默认格式
     */
    PonyAudioFormat getInputFormat() {
        std::unique_lock lock(m_workerLock);
        if (m_worker) {
            return m_worker->getAudioInputFormat();
        } else {
            return PonyPlayer::DEFAULT_AUDIO_FORMAT;
        }

    }

    /**
     * @brief 设置音频输出格式 — 配置重采样器的目标格式
     * @param format 目标音频格式（采样率、通道布局等）
     *
     * 必须保证解码器已停止。设置后需要重新 seek 才能获取到正确格式的音频帧。
     *
     * 设置 demuxer 输出格式, 必须保证 demuxer 已停止, 需要重新 seek 才能保证获取到正确的帧
     * @param format
     */
    void setOutputFormat(PonyAudioFormat format) {
        std::unique_lock lock(m_workerLock);
        m_forward->setAudioOutputFormat(format);
        m_backward->setAudioOutputFormat(std::move(format));
    }


public slots:

    /**
     * @brief 跳转视频进度 — 将解码位置移动到指定时间点
     * @param secs 目标进度（单位：秒）
     *
     * 必须保证解码器线程已暂停且缓冲区已清空。方法返回后保证接下来产生的帧时间戳正确。
     *
     * 一次完整的进度跳转操作:
     *   1. 在 FRAME 线程调用 pause()    — 停止解码器线程
     *   2. 在 FRAME 线程调用 seek()     — 跳转并阻塞等待返回
     *   3. 在 FRAME 线程调用 flush()    — 清空队列中的旧帧
     *   4. 在 DECODER 线程调用 start()  — 恢复解码器运行
     *
     * @see Demuxer::pause
     * @see Demuxer::flush
     * @see DecodeDispatcher::seek
     *
     * 调整视频进度, 必须保证解码器线程空闲且缓冲区为空. 方法返回后保证产生的帧是在时间正确. \n
     * 一次完整的调整进度操作应该为: \n
     * 1. 在VideoThread线程调用 Demuxer2::statePause 使解码器线程停止运行; \n
     * 2. 在VideoThread线程调用 Demuxer2::seek 并阻塞等待函数返回, 接下来产生的帧是新的帧. \n
     * 3. 在VideoThread线程调用 Demuxer2::flush 清空队列中的旧帧. \n
     * 4. 在DeocdeThread线程中执行 Demuxer2::start, 恢复解码器线程线程运行. \n
     * @param secs 视频进度(单位: s)
     * @see Demuxer2::statePause
     * @see Demuxer2::flush
     * @see DecodeDispatcher::seek
     */
    void seek(qreal secs) {
        m_worker->seek(secs);
    }

    /**
     * @brief 设置当前音轨索引
     * @param index 音轨流索引
     *
     * 必须保证解码器线程已暂停且缓冲区为空。
     *
     * @see DecodeDispatcher::seek
     *
     * 设置音频索引, 必须保证解码器线程空闲且缓冲区为空
     * @param index
     * @see DecodeDispatcher::seek
     */
    void setAudioIndex(StreamIndex index) {
        m_forward->setAudioIndex(index);
    }

    /**
     * @brief 打开媒体文件 — 创建正放/倒放解码调度器并启动解码
     * @param fn 本地文件路径（UTF-8 编码）
     *
     * 执行流程:
     *   1. 检查 m_worker 是否为空（已有文件打开则返回 FAILED）
     *   2. 创建 DecodeDispatcher（正放）和 ReverseDecodeDispatcher（倒放）
     *   3. 设置 m_worker 默认指向正向调度器
     *   4. 唤醒解码器线程 (stateResume)
     *   5. 通过 openFileResult 信号返回结果
     *
     * 异常处理: 如果 DecodeDispatcher 构造抛出 std::runtime_error（文件损坏/格式不支持），
     * 清理所有指针并通过信号返回 FAILED。
     *
     * 打开文件
     * @param fn 本地文件路径
     */
    void openFile(const std::string &fn) {
        qDebug() << "Demuxer Open file" << QString::fromUtf8(fn);
        // 在 DECODER 线程执行
        // call on video decoder thread
        std::unique_lock lock(m_workerLock);
        if (m_worker) {
            qWarning() << "Already open file:" << m_worker->filename.c_str();
            // 返回失败
            emit openFileResult(PonyPlayer::OpenFileResultType::FAILED, QPrivateSignal());
            return;
        }
        PonyPlayer::OpenFileResultType result;
        try {
            // 创建正反向解码器
            m_forward = new DecodeDispatcher(fn, result, DEFAULT_STREAM_INDEX, DEFAULT_STREAM_INDEX, this);
            m_backward = new ReverseDecodeDispatcher(fn, this);
            // 默认正向播放
            m_worker = m_forward;
        } catch (std::runtime_error &ex) {
            qWarning() << "Error opening file:" << ex.what();
            m_worker = nullptr;
            m_backward = nullptr;
            m_forward = nullptr;
            emit openFileResult(result, QPrivateSignal());
            return;
        }
        lock.unlock();
        // 恢复解码
        m_worker->stateResume();
        emit openFileResult(result, QPrivateSignal());
        qDebug() << "Open file success.";
    }


    /**
     * @brief 切换到倒放模式 — 将当前活跃调度器指向反向解码器
     *
     * 必须保证解码器线程已暂停且缓冲区为空。
     * 切换后 m_worker 指向 m_backward，并清空正向解码器的缓冲。
     *
     * @see Demuxer::pause
     * @see Demuxer::flush
     * @see DecodeDispatcher::seek
     *
     * 倒放视频, 必须保证解码器线程空闲且缓冲区为空. 方法返回后保证产生的帧是在时间正确.
     * @param secs 视频进度(单位: s)
     * @see Demuxer2::statePause
     * @see Demuxer2::flush
     * @see DecodeDispatcher::seek
     */
    void backward() {
        std::unique_lock lock(m_workerLock);
        m_worker = m_backward;
        m_forward->flush();
    }

    /**
     * @brief 切换到正放模式 — 将当前活跃调度器指向正向解码器
     *
     * 必须保证解码器线程已暂停且缓冲区为空。
     * 切换后 m_worker 指向 m_forward，并清空反向解码器的缓冲。
     *
     * @see backward 对称操作
     *
     * 正向播放视频, 必须保证解码器线程空闲且缓冲区为空. 方法返回后保证产生的帧是在时间正确.
     * @param secs 视频进度(单位: s)
     * @see Demuxer2::statePause
     * @see Demuxer2::flush
     * @see DecodeDispatcher::seek
     */
    void forward() {
        std::unique_lock lock(m_workerLock);
        m_worker = m_forward;
        m_backward->flush();
    };

    /**
     * @brief 关闭当前打开的文件 — 停止解码并销毁正放/倒放调度器
     *
     * 执行流程:
     *   1. 置空 m_worker
     *   2. 暂停正向调度器 (statePause)
     *   3. 通过 deleteLater() 延迟销毁 m_forward
     *   4. 同样处理 m_backward
     *
     * 无文件打开时调用会输出警告。
     */
    void close() {
        std::unique_lock lock(m_workerLock);
        if (m_worker) {
            qDebug() << "Close file" << m_worker->filename.c_str();
            // ① 切断活跃 worker 指针
            m_worker = nullptr;
            if (m_forward) {
                // ② 停止正向解码线程
                m_forward->statePause();
                // ③ 调度删除
                m_forward->deleteLater();
                m_forward = nullptr;
            }
            if (m_backward) {
                // ② 停止反向解码线程
                m_backward->statePause();
                // ③ 调度删除
                m_backward->deleteLater();
                m_backward = nullptr;
            }
        } else {
            qWarning() << "Try to close file while no file has been opened.";
        }
    }

    /**
     * @brief 设置当前播放的音轨
     * @param i 音轨索引
     */
    void setTrack(int i) {
        std::unique_lock lock(m_workerLock);
        m_worker->setTrack(i);
    }

    /**
     * @brief 测试用 — 在工作线程执行测试回调
     */
    void test_onWork() {
        m_worker->test_onWork();
    }

signals:
    /**
     * @brief 文件打开结果信号 — 通知 FrameController 文件打开成功或失败
     * @param result 打开结果（SUCCESS / FAILED）
     *
     * 通过 Qt::QueuedConnection 跨线程传递到 FRAME 线程的 FrameController。
     * 使用 QPrivateSignal 确保仅由 Demuxer 自身发射。
     */
    void openFileResult(PonyPlayer::OpenFileResultType result, QPrivateSignal);
};

