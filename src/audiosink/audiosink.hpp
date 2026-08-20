//
// audiosink.hpp - PonyAudioSink 音频输出后端
//
// 基于 PortAudio 的跨平台音频输出实现：接收解码后的 PCM 数据，
// 经 Sonic 变速/变调/音量处理后写入环形缓冲，由 PortAudio 回调驱动送到声卡。
// 同时负责音频设备枚举、设备切换、热插拔监听与播放位置(时钟)计算。
//
#pragma once

#include "portaudio.h"
#include "ponyplayer.h"
#include <utility>
#include <vector>
#include <QBuffer>
#include <QDebug>
#include "pa_ringbuffer.h"
#include "pa_util.h"
#include "readerwriterqueue.h"
#include "sonic.h"
#include "audioformat.hpp"
#include "private/hotplug.hpp"
#include "ponyplayer.h"
#include <mutex>
#include <shared_mutex>

/// 音频输出状态
enum class PlaybackState {
    PLAYING, ///< 正在播放
    STOPPED, ///< 停止状态
    PAUSED,  ///< 暂停状态
};

/// 断言 PortAudio 调用成功, 失败时打印错误文本并抛出 ILLEGAL_STATE 异常
#define ASSERT_PA_OK(err, message) if ((err) != paNoError) { \
qWarning() << "Error" << Pa_GetErrorText(err); \
ILLEGAL_STATE(message); \
}

/**
 * @brief 播放音频裸流, 用于代替QAudioSink.
 *
 * 这个类的函数都不是线程安全的, 必须保证在 VideoThread 中调用, 这个类的RAII的. 音频播放涉及两个缓存: AudioBuffer
 * 和 DataBuffer. AudioBuffer 由系统维护, 一旦我们向里面写入数据, 我们将不能读取它. 音频播放时, 系统播放 AudioBuffer
 * 中的音频. 当 AudioBuffer 数据不足时, 系统会通过回调函数从 DataBuffer 中获取数据. DataBuffer 由 PonyAudioSink
 * 维护, 当需要播放音频时需要先调用 write 函数将音频数据写入 DataBuffer.
 *
 * 数据流: write(Int16 PCM) → Sonic(变速/变调/音量) → PaUtilRingBuffer → PortAudio 回调 → 声卡
 *
 * 线程模型:
 * - 所有公有方法要求在 PLAYBACK 线程调用(即 Playback::onWork 所在线程);
 * - PortAudio 回调(m_paCallback)在 PortAudio 内部音频线程执行, 通过无锁环形缓冲与写线程解耦;
 * - paStreamLock 保护 PortAudio 全局初始化/流操作(设备切换、重启流)。
 */
class PonyAudioSink : public QObject {
    Q_OBJECT
private:

    PaStream *m_stream{};                       ///< PortAudio 输出流句柄
    PaStreamParameters *param{};                ///< 输出流参数(设备/声道/采样格式/延迟)
    qreal m_volume, m_pitch;                    ///< 音量(0~1) 与音调倍率
    PlaybackState m_state;                      ///< 当前播放状态
    std::atomic<bool> m_pauseRequested = false; // 当播放完缓存的音频后停止
    HotPlugDetector *hotPlugDetector;           ///< 音频设备热插拔监听器(Qt Multimedia)
    QList<QString> devicesList;                 ///< 可用输出设备名列表(供 UI 展示)
    QString selectedOutputDevice;               ///< 当前选中的输出设备名
    inline static std::mutex paStreamLock;      ///< 保护 PortAudio 初始化/流操作的全局锁
    inline static std::atomic_bool paInitialized = false; ///< PortAudio 是否已 Pa_Initialize

    PonyAudioFormat m_format;       ///< 期望的输入格式(强制 Int16, 采样率/声道来自源文件)
    PonyAudioFormat m_deviceFormat; ///< 设备实际协商出的输出格式(Pa_GetStreamInfo 结果)
    size_t m_bufferMaxBytes;        ///< 环形缓冲容量(2 的幂)
    size_t m_sonicBufferMaxBytes;   ///< Sonic 变速输出缓冲容量(= 4 × m_bufferMaxBytes)
    qreal m_speedFactor;            ///< 当前倍速
    PaUtilRingBuffer m_ringBuffer{};    ///< 无锁环形缓冲: 写线程写入, PA 回调线程读取
    std::byte *m_ringBufferData;        ///< 环形缓冲底层内存
    moodycamel::ReaderWriterQueue<AudioDataInfo> dataInfoQueue; ///< 记录每段数据变速前后的长度映射, 用于校准播放位置

    sonicStream sonStream;              ///< Sonic 变速/变调/音量处理流
    std::byte *sonicBuffer = nullptr;   ///< Sonic 输出缓冲

    PaTime m_startPoint = 0.0;          ///< 起始时间戳(秒): 播放位置计算的基准点
    std::atomic<int64_t> m_dataWritten = 0;     ///< 累计写入(折算为 1x 速度)的字节数
    std::atomic<int64_t> m_dataLastWrote = 0;   ///< 最近一次回调写入的(1x 折算)字节数

    std::atomic<bool> m_blockingState = false;  ///< 是否禁用音频输出(倍速过高时置位, 回调输出静音)
    std::mutex m_waitCompleteMutex;             ///< 配合 m_waitCompleteCond
    std::condition_variable m_waitCompleteCond; ///< 播放完缓存数据时唤醒 waitComplete()

    /// PortAudio 流结束回调(数据耗尽时由 PA 内部调用)
    void m_paStreamFinishedCallback() {
        qDebug() << "Stream finished callback.";
        m_state = PlaybackState::PAUSED;
        if (m_state == PlaybackState::PLAYING) {
            emit resourceInsufficient();
        }
        m_waitCompleteCond.notify_all();
        emit stateChanged();

    }

    /**
     * PortAudio 输出回调 — 在 PortAudio 音频线程执行。
     * 从环形缓冲读取数据填满 outputBuffer; 数据不足或音频被禁用时输出静音。
     * 同时通过 dataInfoQueue 把本次实际消费折算成"1x 速度下应有的字节数",
     * 用于在变速播放时仍能准确计算已播放时长。
     */
    int m_paCallback(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer,
                     const PaStreamCallbackTimeInfo *timeInfo) {
        // 环形缓冲中当前可读字节数
        ring_buffer_size_t bytesAvailCount = PaUtil_GetRingBufferReadAvailable(&m_ringBuffer);
        // 本次回调需要输出的字节数 = 帧数 × 单帧字节数(单样本字节 × 声道数)
        auto bytesNeeded = static_cast<ring_buffer_size_t>(framesPerBuffer *
                                                           static_cast<unsigned long>(m_format.getBytesPerSampleChannels()));
        if (m_blockingState) {
            // 音频被禁用(如倍速超过 MAX_SPEED_FACTOR): 只输出静音
            memset(outputBuffer, 0, static_cast<size_t>(bytesNeeded));
        } else if (bytesAvailCount == 0) {
            // 缓冲已空: 输出静音; 若请求了"播放完即停"则结束流
            memset(outputBuffer, 0, static_cast<size_t>(bytesNeeded));
            if (m_pauseRequested) {
                return paComplete;
            } else {
                qWarning() << "paAbort bytesAvailCount == 0";
            }
        } else {
            ring_buffer_size_t timeAlignedByteWritten = 0; // 透明化加速的影响，表示在1x速度下，理应有多少个Byte被写入
            ring_buffer_size_t byteToBeWritten = std::min(bytesNeeded, bytesAvailCount); // 实际要往PortAudio的Buffer里写多少Byte
            ring_buffer_size_t byteRemainToAlign = byteToBeWritten; // 当前还需要处理多少个timeAlignedByteWritten
            // 按 dataInfoQueue 记录的"变速前(origLength)/变速后(processedLength)"映射,
            // 把本次消费的变速后字节数逐段折算回 1x 速度下对应的原始字节数
            while (byteRemainToAlign) {
                auto *audioDataInfo = dataInfoQueue.peek();
                if (audioDataInfo->processedLength < byteRemainToAlign) {
                    // 整段消费: 原始长度全部计入
                    timeAlignedByteWritten += audioDataInfo->origLength;
                    byteRemainToAlign -= audioDataInfo->processedLength;
                    dataInfoQueue.pop();
                } else {
                    // 只消费了段的一部分: 按 speedUpRate 比例折算原始长度
                    ring_buffer_size_t origLengthReduced = std::max(static_cast<ring_buffer_size_t>(1),
                                                                    static_cast<ring_buffer_size_t>(
                                                                            static_cast<double>(byteRemainToAlign) *
                                                                            audioDataInfo->speedUpRate));
                    audioDataInfo->processedLength -= static_cast<qint32>(byteRemainToAlign);
                    audioDataInfo->origLength -= static_cast<qint32>(origLengthReduced);
                    timeAlignedByteWritten += origLengthReduced;
                    byteRemainToAlign = 0;
                }
            }
            if (bytesNeeded > bytesAvailCount) {
                // 缓冲数据不够本次回调: 尽量读, 剩余补静音
                qWarning() << "paAbort bytesAvailCount < bytesNeeded";
                PaUtil_ReadRingBuffer(&m_ringBuffer, outputBuffer, bytesAvailCount);
                memset(static_cast<std::byte *>(outputBuffer) + byteToBeWritten, 0,
                       static_cast<size_t>(bytesNeeded - byteToBeWritten));
            } else {
                PaUtil_ReadRingBuffer(&m_ringBuffer, outputBuffer, byteToBeWritten);
            }
            // 累计 1x 折算后的已播放字节数, 供 getProcessSecs 计算播放位置
            m_dataWritten += timeAlignedByteWritten;
            m_dataLastWrote = timeAlignedByteWritten;
        }
        return paContinue;
    }

    /// 安全重启 PA 流: 先停(忽略未启动), 再启动
    PaError startStreamSafe() {
        PaError err = Pa_StopStream(m_stream);
        if (err != paStreamIsStopped && err != paNoError) {
            return err;
        }
        return Pa_StartStream(m_stream);
    }

    /// 打印 PortAudio 错误信息
    static void printError(PaError error) {
        qDebug() << "Error" << Pa_GetErrorText(error);
    }

    /// 按选中设备名查找设备索引; 找不到则退回默认输出设备
    PaDeviceIndex getCurrentOutputDeviceIndex() {
        int deviceCount = Pa_GetDeviceCount();
        for (auto index = 0; index < deviceCount; index++) {
            auto *device = Pa_GetDeviceInfo(index);
            if (strcmp(device->name, selectedOutputDevice.toStdString().data()) == 0 && device->maxOutputChannels > 0) {
                return index;
            }
        }
        return Pa_GetDefaultOutputDevice();
    }

    // this should be guarded by paStreamLock
    /// 初始化并打开 PortAudio 输出流(需在 paStreamLock 保护下调用)
    void initializeStream() {
        if (!paInitialized) {
            Pa_Initialize();
            paInitialized = true;
            qDebug() << "Initialize PonyAudioSink backend.";
        }
        // 构造参数
        param = new PaStreamParameters;
        if (selectedOutputDevice.isNull()) {
            // 如果没有选择过设备则枚举设备列表
            _getDeviceList();
            // 退回系统默认输出设备
            selectedOutputDevice = Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice())->name;
        }
        // 按设备名字符串在设备表里查找索引
        param->device = getCurrentOutputDeviceIndex();
        // 再用查到的索引回读设备名覆盖 selectedOutputDevice
        selectedOutputDevice = Pa_GetDeviceInfo(param->device)->name;
        param->channelCount = m_format.getChannelCount();
        if (param->device == paNoDevice)
            ILLEGAL_STATE("no audio device!");
        // 通道数
        param->channelCount = m_format.getChannelCount();
        // 采样格式
        param->sampleFormat = m_format.getSampleFormatForPA();
        // 设置低延迟模式
        param->suggestedLatency = Pa_GetDeviceInfo(param->device)->defaultLowOutputLatency;
        // 不使用宿主 API 扩展参数
        param->hostApiSpecificStreamInfo = nullptr;
        // 打开流
        ASSERT_PA_OK(
                Pa_OpenStream(&m_stream, nullptr, param, m_format.getSampleRate(), paFramesPerBufferUnspecified,
                              paClipOff,
                              [](
                                      const void *inputBuffer,
                                      void *outputBuffer,
                                      unsigned long framesPerBuffer,
                                      const PaStreamCallbackTimeInfo *timeInfo,
                                      PaStreamCallbackFlags statusFlags,
                                      void *userData
                              ) {
                                  return static_cast<PonyAudioSink *>(userData)->m_paCallback(inputBuffer, outputBuffer,
                                                                                              framesPerBuffer,
                                                                                              timeInfo);
                              }, this),
                "Can not open audio stream!"
        )
        // 记录设备实际协商出的采样率(可能与请求值不同), 供解码器重采样对齐
        const PaStreamInfo *info = Pa_GetStreamInfo(m_stream);
        m_deviceFormat = PonyAudioFormat(PonyPlayer::Int16, static_cast<int>(info->sampleRate),
                                         param->channelCount);
        // 注册流结束回调
        ASSERT_PA_OK(Pa_SetStreamFinishedCallback(m_stream, [](void *userData) {
            static_cast<PonyAudioSink *>(userData)->m_paStreamFinishedCallback();
        }), "Can not set stream callback!")

    }

    /// 状态枚举 → 字符串(日志用)
    QString stateToStr() {
        switch (m_state) {
            case PlaybackState::PLAYING:
                return "PLAYING";
            case PlaybackState::PAUSED:
                return "PAUSED";
            case PlaybackState::STOPPED:
                return "STOPPED";
        }
        return "UNKNOWN";
    }

    /// 计算大于等于 val 的最小 2 的幂(环形缓冲容量要求 2 的幂)
    static unsigned nextPowerOf2(unsigned val) {
        val--;
        val = (val >> 1) | val;
        val = (val >> 2) | val;
        val = (val >> 4) | val;
        val = (val >> 8) | val;
        val = (val >> 16) | val;
        return ++val;
    }



public:
    /// 最大支持倍速(超过后音频被禁用, 仅按视频同步)
    constexpr const static qreal MAX_SPEED_FACTOR = 4;

    /**
     * 创建PonyAudioSink并attach到默认设备上
     * @param format 音频格式
     * @param bufferSizeAdvice DataBuffer 的建议大小, PonyAudioSink 保证实际的 DataBuffer 不小于建议大小.
     *
     * 初始化流程: 打开 PA 流 → 分配环形缓冲与 Sonic 缓冲 → 创建 Sonic 流 →
     * 创建热插拔监听器(必须在 PA 流打开之后)。
     */
    explicit PonyAudioSink(PonyAudioFormat format) : m_volume(0.5), m_pitch(1.0), m_state(PlaybackState::STOPPED),
                                            m_format(std::move(format)),
                                            m_deviceFormat(PonyPlayer::Unknown, 0, 0), m_speedFactor(1.0) {


        paStreamLock.lock();
        initializeStream();
        paStreamLock.unlock();
        m_bufferMaxBytes = nextPowerOf2(static_cast<unsigned>(m_format.suggestedRingBuffer(MAX_SPEED_FACTOR)));
        m_sonicBufferMaxBytes = m_bufferMaxBytes * 4;
        m_ringBufferData = static_cast<std::byte *>(PaUtil_AllocateMemory(static_cast<long>(m_bufferMaxBytes)));
        if (PaUtil_InitializeRingBuffer(&m_ringBuffer,
                                        sizeof(std::byte),
                                        static_cast<ring_buffer_size_t>(m_bufferMaxBytes),
                                        m_ringBufferData) < 0)
            throw std::runtime_error("can not initialize ring buffer!");
        sonicBuffer = new std::byte[m_sonicBufferMaxBytes];
        sonStream = sonicCreateStream(m_format.getSampleRate(), m_format.getChannelCount());
        sonicSetChordPitch(sonStream, 1);
        sonicSetSpeed(sonStream, static_cast<float>(m_speedFactor));
        // HotPlugDetector should be created after PA stream open
        hotPlugDetector = new HotPlugDetector(this);
        connect(hotPlugDetector, &HotPlugDetector::audioOutputsChanged, this,
                &PonyAudioSink::onAudioOutputDevicesChanged);
    }

    /**
     * 析构即从deattach当前设备
     */
    ~PonyAudioSink() override {
        std::lock_guard lock(paStreamLock);
        m_state = PlaybackState::STOPPED;
        PaError err = Pa_CloseStream(m_stream);
        m_stream = nullptr;
        if (err != paNoError) {
            qWarning() << "Error at Destroying PonyAudioSink" << Pa_GetErrorText(err);
        }
    }

    /**
     * 开始播放, 状态变为 PlaybackState::PLAYING. 若当前DataBuffer内容不足, 状态将会发生改变.
     * @see PonyAudioSink::stateChanged
     * @see PonyAudioSink::resourceInsufficient
     */
    void start() {
        std::lock_guard lock(paStreamLock);
        m_pauseRequested = false;
        qDebug() << "Audio start.";
        if (m_state == PlaybackState::PLAYING) {
            qDebug() << "AudioSink already started.";
            return;
        }
        PaError err = startStreamSafe();
        if (err != paNoError) {
            qWarning() << "Error at starting stream:" << Pa_GetErrorText(err);
            ILLEGAL_STATE("Can not start stream!.");
        }
        m_state = PlaybackState::PLAYING;
        qDebug() << "Pa stream started";

        emit stateChanged();
    }

    /**
     * 暂停播放, 状态变为 PlaybackState::PAUSED. 但已经写入AudioBuffer的音频将会继续播放.
     */
    void pause() {
        std::lock_guard lock(paStreamLock);
        qDebug() << "Audio requesting pause. Current state is " << stateToStr();
        if (m_state == PlaybackState::PLAYING) {
            Pa_StopStream(m_stream);
            qDebug() << "Stream Stopped";
            m_state = PlaybackState::PAUSED;
        } else if (m_state == PlaybackState::STOPPED) {
            // ignore
        } else {
            qWarning() << "AudioSink already paused.";
        }
    }

    /**
     * 阻塞等待音频把缓冲中的数据全部播完。
     * 置 m_pauseRequested, 回调在数据耗尽时返回 paComplete 并通知唤醒。
     * 用于播放到结尾时等待音频自然播完(而非立即切断)。
     */
    void waitComplete() {
        m_pauseRequested = true;
        std::unique_lock lock(m_waitCompleteMutex);
        m_waitCompleteCond.wait(lock);
    }

    /**
     * 停止播放, 状态变为 PlaybackState::STOPPED, 且已写入AudioBuffer的音频将会被放弃, 播放会立即停止.
     */
    void stop() {
        std::lock_guard lock(paStreamLock);
        qDebug() << "Audio stateStop.";
        if (m_state == PlaybackState::PLAYING || m_state == PlaybackState::PAUSED) {
            // 立即终止音频流
            Pa_AbortStream(m_stream);
            m_state = PlaybackState::STOPPED;
        } else {
            qWarning() << "AudioSink already stopped.";
        }
    }

    /// 设置是否禁用音频输出(倍速过高时由上层调用)
    void setBlockState(bool state) {
        m_blockingState = state;
    }

    /// 音频是否处于禁用状态
    bool isBlock() {
        return m_blockingState;
    }

    /**
     * 获取播放状态
     * @return 当前状态
     */
    [[nodiscard]] PlaybackState state() const {
        return m_state;
    }

    /**
     * 获取AudioBuffer剩余空间
     * @return 剩余空间(单位: byte)
     *
     * 减去与 (MAX_SPEED_FACTOR - m_speedFactor) 成正比的一块预留空间:
     * 倍速越高, Sonic 突发产出的数据越多, 需要预留更多余量避免写入失败。
     */
    [[nodiscard]] int64_t freeByte() const {
        return static_cast<int64_t>(PaUtil_GetRingBufferWriteAvailable(&m_ringBuffer))
               - static_cast<int64_t>((MAX_SPEED_FACTOR - m_speedFactor) * static_cast<qreal>(m_bufferMaxBytes) /
                                      MAX_SPEED_FACTOR);
    }

    /**
     * 写AudioBuffer, 要么写入完全成功, 要么失败. 这个操作保证在VideoThread上进行.
     * @param buf 数据源
     * @param origLen 长度(单位: byte)
     * @return 写入是否成功
     *
     * 流程: 仅支持 Int16 → Sonic 变速处理(输出到 sonicBuffer) →
     * 环形缓冲空间不足则整体失败 → 按环形缓冲两个写区段 memcpy →
     * 在 dataInfoQueue 记录 (origLength, processedLength, speedUpRate) 供回调折算播放位置。
     */
    bool write(const char *buf, qint32 origLen) {
        int len = 0;
        if (m_format.getSampleFormat() != PonyPlayer::Int16) {
            throw std::runtime_error("Only support Int16!");
        }
        if (origLen % m_format.getBytesPerSampleChannels() == 0) {
            // Sonic 只接受 short(Int16) 采样; 输入样本数 = 字节数 / 单帧字节数
            sonicWriteShortToStream(sonStream, reinterpret_cast<const short *>(buf),
                                    static_cast<int>(origLen) / m_format.getBytesPerSampleChannels());
            int currentLen;
            // 从 Sonic 流取出变速后的样本(直到取空), 拼接到 sonicBuffer
            while ((currentLen = sonicReadShortFromStream(sonStream,
                                                          reinterpret_cast<short *>
                                                          (sonicBuffer + len * m_format.getChannelCount()),
                                                          0x7fffffff))) {
                len += currentLen;
            }
        } else
            ILLEGAL_STATE("Incomplete Int16!");
        len *= m_format.getBytesPerSampleChannels();
        ring_buffer_size_t bufAvailCount = PaUtil_GetRingBufferWriteAvailable(&m_ringBuffer);

        // 空间不足: 整体失败(不部分写入)
        if (bufAvailCount < len) return false;
        void *ptr[2] = {nullptr};
        ring_buffer_size_t sizes[2] = {0};

        // 环形缓冲可能跨尾部回绕, 拿到至多两个连续写区段
        PaUtil_GetRingBufferWriteRegions(&m_ringBuffer, static_cast<ring_buffer_size_t>(len), &ptr[0], &sizes[0],
                                         &ptr[1],
                                         &sizes[1]);
        memcpy(ptr[0], sonicBuffer, static_cast<size_t>(sizes[0]));
        memcpy(ptr[1], sonicBuffer + sizes[0], static_cast<size_t>(sizes[1]));
        // 记录变速前后长度与比率, 供回调把消费量折算回 1x 速度
        dataInfoQueue.enqueue({origLen, len, static_cast<qreal>(origLen) / len});
        PaUtil_AdvanceRingBufferWriteIndex(&m_ringBuffer, static_cast<ring_buffer_size_t>(len));
        return true;
    }

    /**
     * 清空AudioBuffer, 将所有空间标记为可用. 这个操作保证在VideoThread上进行.
     * @return 清空数据长度(单位: byte)
     */
    size_t clear() {
        if (m_state != PlaybackState::STOPPED) {
            qWarning() << "clear make no effect when state != STOPPED.";
        }
        // 需要保证此刻没有读写操作
        // 丢弃缓冲区内所有待播放的音频数据
        PaUtil_FlushRingBuffer(&m_ringBuffer);
        return 0;
    }


    /**
     * 获取当前播放的时间, 这个函数只能在 PlaybackState::PLAYING 或 PlaybackState::PAUSED 状态下使用.
     * @return 当前已播放音频的长度(单位: 秒)
     *
     * 位置 = m_startPoint ± (已播放的 1x 折算字节数 → 秒)。
     * 用 m_dataWritten - m_dataLastWrote 排除"已写入环形缓冲但尚未被回调消费"的部分,
     * 得到真正播出到声卡的时长; 倒放时从起始点往回减。
     */
    [[nodiscard]] qreal getProcessSecs(bool backward) const {
        if (m_state == PlaybackState::STOPPED) { return m_startPoint; }
        auto processSec = static_cast<double>(m_format.durationOfBytes(m_dataWritten - m_dataLastWrote));
        if (backward) {
            return m_startPoint - processSec;
        } else {
            return m_startPoint + processSec;
        }
    }

    /**
     * 设置下一次播放的计时器. 这个函数必须在 PlaybackState::STOPPED 状态下使用. 在播放开始后, 设置生效。
     * @param t 新的播放时间(单位: 秒)
     *
     * 同时清零已写入字节计数, 使 getProcessSecs 从新起点开始累计。
     */
    void setStartPoint(double t = 0.0) {
        if (std::isnan(t)) {
            qWarning() << "Trying set start point to NaN";
        }
        if (m_state == PlaybackState::STOPPED) {
            // 设置起始时间戳
            m_startPoint = t;
            // 清零已写入字节计数
            m_dataWritten = 0;
        } else {
            qWarning() << "setTimeBase make no effect when state != STOPPED";
        }
    }


    /**
     * 设备音量, 音量的范围通常是[0, 1]
     * @param newVolume
     */
    void setVolume(qreal newVolume) {
        m_volume = qBound(0.0, newVolume, 1.0);
        sonicSetVolume(sonStream, static_cast<float>(newVolume));
    }

    /**
     * 设置音调
     * @param newPitch
     */
    void setPitch(qreal newPitch) {
        m_pitch = qBound(0.0, newPitch, 16.0);
        sonicSetPitch(sonStream, static_cast<float>(newPitch));
    }

    /**
     * 设置速度
     * @param newSpeed
     */
    void setSpeed(qreal newSpeed) {
        m_speedFactor = qBound(0.0, newSpeed, MAX_SPEED_FACTOR);
        sonicSetSpeed(sonStream, static_cast<float>(newSpeed));
    }

    /**
     * 获取当前音量
     * @return
     */
    [[nodiscard]] qreal volume() const {
        return m_volume;
    }

    /// 获取当前倍速
    [[nodiscard]] qreal speed() const {
        return m_speedFactor;
    }

    /// 获取当前音调倍率
    [[nodiscard]] qreal pitch() const {
        return m_pitch;
    }

    /// 枚举所有可用输出设备(Windows 下仅保留 DirectSound 宿主 API 的设备)
    void _getDeviceList() {
        // 清空原有的设备列表容器
        devicesList.clear();
        // 获取当前系统检测到的所有音频设备（输入和输出）的总数量
        int devicesCount = Pa_GetDeviceCount();
        for (auto index = 0; index < devicesCount; index++) {
            // 获取当前索引设备的详细信息结构体指针
            auto deviceInfo = Pa_GetDeviceInfo(index);
            // 提取设备的名称
            QString deviceName = Pa_GetDeviceInfo(index)->name;
            // 检查设备的最大输出声道数。如果小于 2（即不支持立体声输出，比如纯单声道设备或纯录音麦克风），则跳过该设备
            if (deviceInfo->maxOutputChannels < 2) continue;
            // 检查音频底层 API 驱动类型。如果不是 DirectSound 驱动（例如 WASAPI、MME、ASIO 等），则跳过该设备
#ifdef WIN32
            if (deviceInfo->hostApi != PaHostApiTypeId::paDirectSound) continue;
#endif
            devicesList.push_back(deviceName);
            qDebug() << deviceName;
        }
    }

    /// 获取当前选中的输出设备名
    QString getSelectedOutputDevice() { return selectedOutputDevice; }

    /**
     * 重启输出流: 终止并重新初始化 PortAudio 后端。
     * @param betweenInitAndOpen 在重新 Pa_Initialize 之后、打开流之前执行的回调
     *                          (如刷新设备列表), 可为空。
     * 若当前正在播放, 重启后自动恢复播放。
     */
    void restartStream(const std::function<void()> &betweenInitAndOpen) {
        // 加锁
        std::lock_guard lock(paStreamLock);
        // 若后端已初始化
        if (paInitialized) {
            // 立即中止旧流（丢弃缓冲）
            Pa_AbortStream(m_stream);
            // 整个 PortAudio 后端关停
            Pa_Terminate();
        }
        // 重新初始化后端
        Pa_Initialize();
        paInitialized = true;
        // 可选的钩子函数
        if (betweenInitAndOpen) betweenInitAndOpen();
        // 用当前参数重建流
        initializeStream();
        // 播放中则自动恢复
        if (m_state == PlaybackState::PLAYING) {
            startStreamSafe();
        }
    }

    /// 刷新设备列表并重启流(热插拔或设备列表变化时调用)
    void refreshDevicesList() {
        qDebug() << "Refreshing Devices list...";
        auto middleFunc = [this] {
            _getDeviceList();
        };
        restartStream(middleFunc);
        emit signalAudioOutputDeviceListChanged();
        emit signalDeviceSwitched();
    }

    /// 获取可用输出设备名列表(供 UI 展示)
    QStringList getAudioDeviceList() {
        return devicesList;
    }

    /**
     * 设置音频输入格式并重启流。
     * 强制采样格式为 Int16, 采样率/声道取自传入格式(通常是源文件格式);
     * 流以该参数重新打开, 设备实际协商结果记录在 m_deviceFormat。
     */
    void setFormat(const PonyAudioFormat &format) {
        std::unique_lock lock(paStreamLock);
        m_format = {PonyPlayer::Int16, format.getSampleRate(), format.getChannelCount()};
        lock.unlock();
        restartStream(nullptr);
    }


signals:

    /**
     * 播放状态发生改变
     */
    void stateChanged();

    /**
     * 由于缺少音频数据, 被迫暂停播放
     */
    void resourceInsufficient();

    /// 音频输出设备列表发生变化(热插拔/刷新后)
    void signalAudioOutputDeviceListChanged();

    /// 音频输出设备已切换
    void signalDeviceSwitched();

public slots:

    /// 设备热插拔事件: 刷新设备列表并重启流
    void onAudioOutputDevicesChanged() {
        refreshDevicesList();
    }

    /**
     * 切换音频输出设备。
     * 终止旧流 → 重新 Pa_Initialize → 按新设备名打开流;
     * 若正在播放则恢复播放, 最后发出 signalDeviceSwitched 通知上层。
     */
    void requestDeviceSwitch(const QString &device) {
        qDebug() << "change audio output device to " << device;
        selectedOutputDevice = device;
        std::unique_lock lock(paStreamLock);
        if (paInitialized) {
            Pa_AbortStream(m_stream);
            Pa_Terminate();
        }
        paInitialized = false;
        initializeStream();
        if (m_state == PlaybackState::PLAYING) {
            startStreamSafe();
        }
        lock.unlock();
        emit signalDeviceSwitched();
    }

    /// 获取设备实际协商出的输出格式(供解码器重采样对齐)
    PonyAudioFormat getCurrentDeviceFormat() {
        return m_deviceFormat;
    }
};
