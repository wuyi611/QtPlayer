/**
 * @file dispatcher.hpp
 * @brief 解码调度器 — 负责把解封装得到的 Packet 交给解码器解码成 Frame, 并通过联动队列提供给消费线程
 *
 * 本文件包含四部分内容:
 *   1. StreamInfo — 单个流的信息封装 (时长 / 元数据 / 友好名称)
 *   2. DemuxDispatcherBase — 调度器抽象基类, 定义与打开文件生命周期一致的公共接口
 *   3. DecodeDispatcher — 正向解码调度器 (正常播放方向)
 *   4. ReverseDecodeDispatcher — 反向解码调度器 (倒放方向)
 *
 * 线程模型:
 *   - 调度器对象常驻 DECODER 线程; 通过 QueuedConnection 把 signalStartWorker 信号连接到
 *     onWork 槽, 因此解封装 + 解码主循环 onWork 始终在 DECODER 线程执行.
 *   - 解码出的 AVFrame 写入 TwinsBlockQueue (音频/视频共用同一把锁与条件变量的联动队列),
 *     消费线程 (FRAME 线程 / 音频线程) 通过 getPicture / getSample 阻塞读取.
 *   - 控制接口 (statePause / stateResume / flush / seek) 标注 PONY_GUARD_BY(FRAME),
 *     表示需在 FRAME 线程调用; 帧获取接口标注 PONY_THREAD_SAFE, 可跨线程调用.
 *
 * @see Demuxer 上层封装
 */
//
// Created by ColorsWind on 2022/4/30.
// Adapted from demuxer v1 by kurisu on 2022/4/16.
//
#pragma once

#include <QtCore>
#include <QTimer>
#include <unordered_map>
#include <vector>
#include "ponyplayer.h"
#include "helper.hpp"
#include "frame.hpp"
#include "twins_queue.hpp"
#include "forward.hpp"
#include "backward.hpp"
#include "audioformat.hpp"
#include "virtual.hpp"

INCLUDE_FFMPEG_BEGIN
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
INCLUDE_FFMPEG_END

//#define IGNORE_VIDEO_FRAME  // (调试用) 定义后忽略视频帧

namespace PonyPlayer {
    Q_NAMESPACE
    enum OpenFileResultType {
        FAILED,        ///< 打开文件失败
        VIDEO,         ///< 打开的文件为视频文件
        AUDIO          ///< 打开的文件为音频文件
    };

    Q_ENUM_NS(OpenFileResultType)
}


/**
 * @brief 流信息封装 — 记录单个媒体流 (AVStream) 的索引、元数据标签与时长
 *
 * 构造时从 AVStream 中拷贝出全部元数据 (如 language) 并计算以秒为单位的时长,
 * 生命周期随所属调度器.
 */
class StreamInfo {
private:
    /// 流在容器中的索引 (对应 fmtCtx->streams 的下标)
    int index;
    /// 流的元数据标签表 (key = 标签名, value = 标签值)
    std::unordered_map<std::string, std::string> dict;
    /// 流时长 (单位: 秒)
    qreal duration;
public:
    /// 从 AVStream 构造, 拷贝元数据并换算时长
    explicit StreamInfo(AVStream *stream) : index(stream->index) {
        AVDictionaryEntry *tag = nullptr;
        while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            dict[tag->key] = tag->value;
        }
        duration = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    /// 获取流时长 (秒)
    [[nodiscard]] qreal getDuration() const { return duration; }

    /// 生成用于展示的友好名称, 形如 "language | 1m3.5s"
    [[nodiscard]] QString getFriendName() const {
        QString str;
        if (auto iter = dict.find("language"); iter != dict.cend()) {
            str.append(iter->second.c_str());
            str.append(" | ");
        }
        auto minutes = static_cast<int>(duration) / 60;
        auto seconds = duration - minutes * 60;
        if (minutes > 0) { str.append(QString::number(minutes)).append("m"); }
        str.append(QString::number(seconds, 'f', 1).append("s"));
        return str;
    }

    [[nodiscard]] int getIndex() const { return index; }

};

/// 流索引类型
typedef unsigned int StreamIndex;
/// 默认流索引 — 表示"未指定", 调度器会自动选择第一个可用的同类流
constexpr StreamIndex DEFAULT_STREAM_INDEX = std::numeric_limits<StreamIndex>::max();

/**
 * @brief 解码调度器抽象基类 — 持有打开文件的解封装上下文, 定义统一的调度接口
 *
 * 生命周期与打开的文件相同: 构造时打开文件并读取流信息, 析构时关闭文件.
 * 视频/音频结果通过成员方法提供给消费线程, 具体编解码与缓冲策略由子类实现.
 */
class DemuxDispatcherBase : public QObject {
Q_OBJECT
public:
    /// 已打开文件的路径
    const std::string filename;
protected:
    /// FFmpeg 解封装上下文 (容器级别的 demux 状态)
    AVFormatContext *fmtCtx = nullptr;
    /// 是否为纯音频文件 (由扩展名判定, 见构造函数)
    bool isAudio = false;

    /**
     * @brief 打开文件并读取流信息
     * @param fn 本地文件路径
     * @throw std::runtime_error 文件无法打开或找不到任何流
     *
     * 先根据扩展名粗略判断是否为纯音频文件 (仅影响是否创建虚拟视频解码器),
     * 再调用 avformat_open_input / avformat_find_stream_info 完成解封装初始化.
     */
    explicit DemuxDispatcherBase(const std::string &fn, QObject *parent) : QObject(parent), filename(fn) {
        // 通过文件扩展名预判是否为纯音频文件
        auto surfix = fn.substr(fn.rfind('.')+1);
        if (surfix == "mp3" || surfix == "wav")
            isAudio = true;
        // 打开输入文件 (仅探测容器格式, 不打开具体解码器)
        if (avformat_open_input(&fmtCtx, fn.c_str(), nullptr, nullptr) < 0) {
            throw std::runtime_error("Cannot open input file.");
        }
        // 读取容器内全部流的编解码信息
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            throw std::runtime_error("Cannot find any stream in file.");
        }
    }

    /// 析构 — 关闭文件释放解封装上下文
    ~DemuxDispatcherBase() override {
        if (fmtCtx) { avformat_close_input(&fmtCtx); }
    }

public:
    /// 暂停解码: 唤醒所有阻塞在队列上的线程, 并请求解码循环尽快停止
    PONY_GUARD_BY(FRAME)

    virtual void statePause() {NOT_IMPLEMENT_YET}

    /// 清空队列中所有未消费的旧帧
    PONY_GUARD_BY(FRAME)

    virtual void flush() {NOT_IMPLEMENT_YET}

    /// 恢复解码: 重新打开队列并启动解码循环
    PONY_GUARD_BY(FRAME)

    virtual void stateResume() {NOT_IMPLEMENT_YET}

    /// 跳转到指定时间点, 之后产生的帧时间戳应落在该时间点之后
    PONY_GUARD_BY(FRAME)

    virtual void seek(qreal secs) {NOT_IMPLEMENT_YET}

    /// 取出队首视频帧并移除 (线程安全)
    PONY_THREAD_SAFE virtual VideoFrameRef getPicture() {NOT_IMPLEMENT_YET}

    /// 查看队首视频帧的时间戳, 不弹出队列 (线程安全)
    PONY_THREAD_SAFE virtual qreal frontPicture() {NOT_IMPLEMENT_YET}

    /// 跳过队首满足条件的视频帧, 返回实际跳过的帧数
    virtual int skipPicture(const std::function<bool(qreal)> &function) {NOT_IMPLEMENT_YET}

    /// 取出队首音频帧并移除 (线程安全)
    PONY_THREAD_SAFE virtual AudioFrame getSample() {NOT_IMPLEMENT_YET}

    /// 查看队首音频帧的时间戳, 不弹出队列 (线程安全)
    PONY_THREAD_SAFE virtual qreal frontSample() {NOT_IMPLEMENT_YET}

    /// 跳过队首满足条件的音频帧, 返回实际跳过的帧数
    virtual int skipSample(const std::function<bool(qreal)> &function) {NOT_IMPLEMENT_YET}

    /// 切换音轨 (仅切换音频解码器, 需在解码空闲时调用)
    virtual void setTrack(int i) {NOT_IMPLEMENT_YET}

    /// 当前文件是否包含视频轨
    virtual bool hasVideo() {NOT_IMPLEMENT_YET}

    /// 启用/禁用音频解码输出
    virtual void setEnableAudio(bool enable) {NOT_IMPLEMENT_YET}

    /// 获取音频解码器的输入格式 (源采样率/通道数等)
    virtual PonyAudioFormat getAudioInputFormat() = 0;

    /// 设置音频解码器的目标输出格式 (重采样目标)
    virtual void setAudioOutputFormat(PonyAudioFormat format) = 0;

    /// 测试接口: 在当前线程同步执行一次解码循环
    virtual void test_onWork() = 0;
};

/**
 * @brief 正向解码调度器 — 正常播放方向: 读 Packet -> 解码 -> 入队
 *
 * 工作流程:
 *   1. 构造时选择音频/视频流并创建对应解码器与联动队列
 *   2. stateResume() 通过信号在 DECODER 线程启动 onWork() 解码循环
 *   3. onWork() 循环 av_read_frame 读取 Packet, 按流索引分发给对应解码器
 *   4. 解码器把 AVFrame 压入联动队列, 消费线程阻塞读取
 *
 * 对象为 RAII: 构造时打开文件并创建资源, 析构时关闭文件并释放全部资源.
 */
class DecodeDispatcher : public DemuxDispatcherBase {
Q_OBJECT
private:
    /// 文件描述信息: 音视频时长 / 各类流索引 / 流信息表
    struct {
        qreal videoDuration = std::numeric_limits<qreal>::quiet_NaN(); ///< 视频流时长 (秒)
        qreal audioDuration = std::numeric_limits<qreal>::quiet_NaN(); ///< 音频流时长 (秒)
        std::vector<StreamIndex> m_videoStreamsIndex; ///< 容器内全部视频流索引
        std::vector<StreamIndex> m_audioStreamsIndex; ///< 容器内全部音频流索引
        std::vector<StreamInfo> streamInfos;          ///< 各流的详细信息
    } description;

    TwinsBlockQueue<AVFrame *> *videoQueue;  ///< 视频帧联动队列 (audioQueue 的孪生)
    TwinsBlockQueue<AVFrame *> *audioQueue;  ///< 音频帧联动队列
    StreamIndex m_audioStreamIndex;          ///< 当前选中的音频流索引
    StreamIndex m_videoStreamIndex;          ///< 当前选中的视频流索引
    IDemuxDecoder *m_audioDecoder;           ///< 音频解码器
    IDemuxDecoder *videoDecoder;             ///< 视频解码器 (纯音频文件时为 VirtualVideoDecoder)
    std::atomic<bool> interrupt = true;      ///< 解码循环终止标志; true 表示停止
    AVPacket *packet = nullptr;              ///< 复用的解封装 Packet 缓冲区

public:
    /**
     * @brief 打开文件, 选择音视频流并构建解码管线
     * @param fn 文件路径
     * @param result 输出参数, 返回打开结果 (FAILED / VIDEO / AUDIO)
     * @param audioStreamIndex 指定音频流索引, 默认 DEFAULT_STREAM_INDEX 表示自动选择
     * @param videoStreamIndex 指定视频流索引, 默认 DEFAULT_STREAM_INDEX 表示自动选择
     * @throw std::runtime_error 文件中不存在音频流时抛出并置 result 为 FAILED
     */
    explicit DecodeDispatcher(
            const std::string &fn,
            PonyPlayer::OpenFileResultType &result,
            StreamIndex audioStreamIndex = DEFAULT_STREAM_INDEX,
            StreamIndex videoStreamIndex = DEFAULT_STREAM_INDEX,
            QObject *parent = nullptr
    ) : DemuxDispatcherBase(fn, parent), m_audioStreamIndex(audioStreamIndex), m_videoStreamIndex(videoStreamIndex) {
        // 复用同一块 Packet 缓冲区, 循环中每次读取后都要 unref
        packet = av_packet_alloc();
        // 遍历所有流, 分类统计视频/音频流并收集流信息
        for (StreamIndex i = 0; i < fmtCtx->nb_streams; ++i) {
            auto *stream = fmtCtx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                description.m_videoStreamsIndex.emplace_back(i);
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                description.m_audioStreamsIndex.emplace_back(i);
            }
            StreamInfo info(stream);
            description.streamInfos.emplace_back(stream);
        }

        // ===== 音频管线的初始化 =====
        // 音频流是必需项: 找不到则判定打开失败
        if (description.m_audioStreamsIndex.empty()) {
            result = PonyPlayer::OpenFileResultType::FAILED;
            throw std::runtime_error("Cannot find audio stream.");
        }
        // 未指定音频流索引时, 默认选第一个音频流
        if (m_audioStreamIndex ==
            DEFAULT_STREAM_INDEX) { m_audioStreamIndex = description.m_audioStreamsIndex.front(); }
        // 音频帧队列容量 16, 采用正向解码器
        audioQueue = new TwinsBlockQueue<AVFrame *>("AudioQueue", 16);
        // 正向音频解码器
        m_audioDecoder = new DecoderImpl<Audio>(fmtCtx->streams[m_audioStreamIndex], audioQueue);
        // 音频流时长
        description.audioDuration = m_audioDecoder->duration();

        // ===== 视频管线的初始化 =====
        // 视频队列是音频队列的孪生: 二者共享同一把锁 / 条件变量 / 开关状态
        videoQueue = audioQueue->twins("VideoQueue", 16);
        if (isAudio) {
            // 纯音频文件: 没有真实视频帧, 用一个"虚拟"视频解码器恒定输出 NaN 时间戳
            // no video
            qDebug() << "audio only";
            if (!description.m_videoStreamsIndex.empty())
                m_videoStreamIndex = description.m_videoStreamsIndex.front();
            videoDecoder = new VirtualVideoDecoder(description.audioDuration);
            result = PonyPlayer::OpenFileResultType::AUDIO;
        } else {
            // 视频文件: 选择视频流并创建真正的视频解码器
            result = PonyPlayer::OpenFileResultType::VIDEO;
            if (m_videoStreamIndex ==
                DEFAULT_STREAM_INDEX) { m_videoStreamIndex = description.m_videoStreamsIndex.front(); }
            // 视频正向解码器
            videoDecoder = new DecoderImpl<Video>(fmtCtx->streams[m_videoStreamIndex], videoQueue);
        }
        description.videoDuration = videoDecoder->duration();
        // onWork 在调度器所在线程 (DECODER 线程) 执行
        connect(this, &DecodeDispatcher::signalStartWorker, this, &DecodeDispatcher::onWork, Qt::QueuedConnection);
    }

    /// 析构 — 清空残留帧后释放全部资源
    ~DecodeDispatcher() override {
        qDebug() << "Destroy decode dispatcher " << filename.c_str();
        DecodeDispatcher::flush();
        delete audioQueue;
        delete videoQueue;
        delete m_audioDecoder;
        delete videoDecoder;
        if (packet) { av_packet_free(&packet); }
    }

    /**
     * 保证阻塞获取结果的线程尽快被唤醒, 同时请求 DecodeThread 的工作尽快停止.
     */
    void statePause() override {
        interrupt = true;
        videoQueue->close();
        qDebug() << "Queue close.";
    }

    /// 清空音频/视频队列中所有未消费的旧帧 (释放每个 AVFrame)
    void flush() override {
        videoQueue->clear([](AVFrame *frame) { av_frame_free(&frame); });
        audioQueue->clear([](AVFrame *frame) { av_frame_free(&frame); });
    }


    /**
     * 保证可以阻塞地获取 Picture 和 Sample. 这个方法是线程安全的.
     * 恢复解码
     */
    void stateResume() override {
        if (interrupt.exchange(false)) {
            // 打开视频队列
            videoQueue->open();
            emit signalStartWorker(QPrivateSignal());
            qDebug() << "Queue stateResume.";
        }
        // ignore when already start

    }

    /**
     * 修改视频播放进度, 注意: 这个方法必须在解码线程上调用.
     * @param secs 新的视频进度(单位: 秒)
     */
    void seek(qreal secs) override {
        // case 1: currently decoding, interrupt
        // case 2: not decoding, seek
        interrupt = true;
        qDebug() << "a Seek:" << secs;
        int ret = av_seek_frame(fmtCtx, -1, static_cast<int64_t>(secs * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        if (m_audioDecoder) { m_audioDecoder->flushFFmpegBuffers(); }
        if (videoDecoder) { videoDecoder->flushFFmpegBuffers(); }
        if (ret != 0) { qWarning() << "Error av_seek_frame:" << ffmpegErrToString(ret); }
    }

    /// 取出队首视频帧 (线程安全, 内部走联动队列)
    PONY_THREAD_SAFE VideoFrameRef getPicture() override { return videoDecoder->getPicture(); }

    /// 查看队首视频帧的时间戳, 不弹出 (线程安全)
    PONY_THREAD_SAFE qreal frontPicture() override { return videoDecoder->viewFront(); }

    /// 跳过队首满足 predicate 条件的视频帧
    PONY_THREAD_SAFE int skipPicture(const std::function<bool(qreal)> &predicate) override {
        return videoDecoder->skip(predicate);
    }

    /// 取出队首音频帧 (线程安全, 内部走联动队列)
    PONY_THREAD_SAFE AudioFrame getSample() override { return m_audioDecoder->getSample(); }

    /// 查看队首音频帧的时间戳, 不弹出 (线程安全)
    PONY_THREAD_SAFE qreal frontSample() override { return m_audioDecoder->viewFront(); }

    /// 跳过队首满足 predicate 条件的音频帧
    PONY_THREAD_SAFE int skipSample(const std::function<bool(qreal)> &predicate) override {
        return m_audioDecoder->skip(predicate);
    }

    /// 获取音频总时长 (秒)
    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    [[nodiscard]] qreal getAudionLength() const { return description.audioDuration; }

    /// 获取视频总时长 (秒)
    PONY_GUARD_BY(MAIN, FRAME, DECODER)

    [[nodiscard]] qreal getVideoLength() const { return description.videoDuration; }

    /// 按音轨序号切换音频解码器 (需在解码空闲时调用)
    PONY_GUARD_BY(DECODER)

    void setTrack(int i) override {
        delete m_audioDecoder;
        m_audioStreamIndex = description.m_audioStreamsIndex[static_cast<size_t>(i)];
        auto stream = fmtCtx->streams[m_audioStreamIndex];
        m_audioDecoder = new DecoderImpl<Audio>(stream, audioQueue);
    }

    /// 返回全部音轨的友好名称列表 (供 UI 展示)
    PONY_GUARD_BY(DECODER)

    QStringList getTracks() {
        QStringList ret;
        ret.reserve(static_cast<qsizetype>(description.m_audioStreamsIndex.size()));
        for (auto &&i: description.m_audioStreamsIndex) {
            ret.emplace_back(description.streamInfos[i].getFriendName());
        }
        return ret;
    }

    /// 按流索引切换音频解码器 (与 setTrack 等价, 直接指定流索引)
    PONY_GUARD_BY(DECODER)

    void setAudioIndex(StreamIndex i) {
        if (i == m_audioStreamIndex) { return; }
        delete m_audioDecoder;
        m_audioStreamIndex = i;
        m_audioDecoder = new DecoderImpl<Audio>(fmtCtx->streams[m_audioStreamIndex], audioQueue);
    }

    /// 当前文件是否包含视频轨
    PONY_GUARD_BY(DECODER)

    bool hasVideo() override {
        return !isAudio;
    }

    /// 启用/禁用音频输出
    PONY_GUARD_BY(DECODER)

    void setEnableAudio(bool enable) override {
        m_audioDecoder->setEnable(enable);
    }

    /// 获取音频解码器输入格式 (源文件采样格式)
    PonyAudioFormat getAudioInputFormat() override {
        return m_audioDecoder->getInputFormat();
    }

    /// 设置音频输出格式 (重采样目标)
    void setAudioOutputFormat(PonyAudioFormat format) override {
        m_audioDecoder->setOutputFormat(format);
    }

    /// 测试接口: 同步执行一次解码循环 (调试用)
    void test_onWork() override {
        onWork();
    }

private slots:

    /// 正向解码主循环 — 在 DECODER 线程执行: 读 Packet 并分发解码, 结果入队
    void onWork() {
        // 打开队列开关, 允许解码结果入队
        videoQueue->open();
        while (!interrupt) {
            // 每次读取一个 Packet, 按流索引分发给对应解码器
            int ret = av_read_frame(fmtCtx, packet);
            if (ret == 0) {
                if (static_cast<StreamIndex>(packet->stream_index) == m_videoStreamIndex) {
                    videoDecoder->accept(packet, interrupt);
                } else if (static_cast<StreamIndex>(packet->stream_index) == m_audioStreamIndex) {
                    m_audioDecoder->accept(packet, interrupt);
                }
            } else if (ret == ERROR_EOF) {
                // 读到文件末尾: 以空指针标记 EOF 通知消费线程, 结束循环
                videoQueue->push(nullptr);
                audioQueue->push(nullptr);
                av_packet_unref(packet);
                break;
            } else {
                // 读取失败 (如数据损坏)
                qWarning() << "Error av_read_frame:" << ffmpegErrToString(ret);
            }
            av_packet_unref(packet);
        }
        interrupt = true;
    };


signals:

    void signalStartWorker(QPrivateSignal);
};

/**
 * @brief 反向解码调度器 — 倒放方向: 分段向前解码, 在内存中倒序输出
 *
 * FFmpeg 只支持正向解码, 因此倒放通过"分段解码"实现:
 *   1. 每次解码一小段 [from-interval, from) 内的帧, 存入各解码器的 frameStack
 *   2. 当 follower 追上 from 时, pushFrameStack 把整段帧倒序压入队列 (见 ReverseDecoderImpl::accept)
 *   3. 上一段消费完毕后, 把 from 前移一个 interval 并重新 seek 到更早的位置继续解码
 *
 * 分段推进的节奏由 primary 解码器 (视频文件为视频解码器, 纯音频文件为音频解码器)
 * 的 nextSegment() 决定, 见 onWork().
 */
class ReverseDecodeDispatcher : public DemuxDispatcherBase {
Q_OBJECT
    PONY_THREAD_AFFINITY(DECODER)
private:
    /// 文件描述信息 (同正向调度器)
    struct {
        qreal videoDuration = std::numeric_limits<qreal>::quiet_NaN(); ///< 视频流时长 (秒)
        qreal audioDuration = std::numeric_limits<qreal>::quiet_NaN(); ///< 音频流时长 (秒)
        std::vector<StreamIndex> m_videoStreamsIndex; ///< 全部视频流索引
        std::vector<StreamIndex> m_audioStreamsIndex; ///< 全部音频流索引
        std::vector<StreamInfo> streamInfos;          ///< 各流详细信息
    } description;

    StreamIndex m_audioStreamIndex;  ///< 当前音频流索引
    StreamIndex m_videoStreamIndex;  ///< 当前视频流索引
    const qreal interval = 5.0;      ///< 反向解码每个分段的长度 (秒), 也是 seek 的提前量

    AVStream *videoStream{};         ///< (预留) 视频流指针, 当前未使用
    AVStream *audioStream{};         ///< (预留) 音频流指针, 当前未使用

    IDemuxDecoder *videoDecoder;     ///< 视频解码器 (纯音频文件时为 VirtualVideoDecoder)
    IDemuxDecoder *m_audioDecoder;   ///< 音频解码器
    IDemuxDecoder *primary;          ///< 主导分段推进的解码器, 其 nextSegment() 驱动 onWork 跳转

    std::atomic<bool> interrupt = true; ///< 解码循环终止标志
    AVPacket *packet = nullptr;      ///< 复用的 Packet 缓冲区
    TwinsBlockQueue<AVFrame *> *videoQueue; ///< 视频帧联动队列
    TwinsBlockQueue<AVFrame *> *audioQueue; ///< 音频帧联动队列
public:
    /**
     * @brief 打开文件并构建反向解码管线
     * @param fn 文件路径
     * @param parent 父 QObject
     * @throw std::runtime_error 文件中不存在音频流时抛出
     */
    explicit ReverseDecodeDispatcher(const std::string &fn,
                                     QObject *parent = nullptr
    ) : DemuxDispatcherBase(fn, parent), m_audioStreamIndex(DEFAULT_STREAM_INDEX), m_videoStreamIndex(DEFAULT_STREAM_INDEX) {
        // 复用 Packet 缓冲区
        packet = av_packet_alloc();
        // 遍历流并分类统计 (与正向调度器一致)
        for (StreamIndex i = 0; i < fmtCtx->nb_streams; ++i) {
            auto *stream = fmtCtx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                description.m_videoStreamsIndex.emplace_back(i);
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                description.m_audioStreamsIndex.emplace_back(i);
            }
            StreamInfo info(stream);
            description.streamInfos.emplace_back(stream);
        }

        // ===== 音频管线 =====
        // 音频流必需; 队列容量更大 (200), 因为反向解码需要预存一段帧
        if (description.m_audioStreamsIndex.empty()) {
            throw std::runtime_error("Cannot find audio stream.");
        }
        if (m_audioStreamIndex ==
            DEFAULT_STREAM_INDEX) { m_audioStreamIndex = description.m_audioStreamsIndex.front(); }

        audioQueue = new TwinsBlockQueue<AVFrame *>("AudioQueue", 200);

        m_audioDecoder = new ReverseDecoderImpl<Audio>(fmtCtx->streams[m_audioStreamIndex], audioQueue);
        description.audioDuration = m_audioDecoder->duration();

        // ===== 视频管线 =====
        videoQueue = audioQueue->twins("VideoQueue", 200);
        if (isAudio) {
            // 纯音频文件: 虚拟视频解码器, 且禁用视频队列; 音频解码器跟随自己作为主导
            // no video
            qDebug() << "audio only";
            if (!description.m_videoStreamsIndex.empty())
                m_videoStreamIndex = description.m_videoStreamsIndex.front();
            videoDecoder = new VirtualVideoDecoder(description.audioDuration);
            videoQueue->setEnable(false);
            m_audioDecoder->setFollower(m_audioDecoder);
            primary = m_audioDecoder;
        } else {
            // 视频文件: 视频解码器主导分段, 音频解码器作为其 follower 同步推进
            if (m_videoStreamIndex ==
                DEFAULT_STREAM_INDEX) { m_videoStreamIndex = description.m_videoStreamsIndex.front(); }
            videoDecoder = new ReverseDecoderImpl<Video>(fmtCtx->streams[m_videoStreamIndex], videoQueue);
            videoDecoder->setFollower(m_audioDecoder);
            primary = videoDecoder;
        }
        description.videoDuration = videoDecoder->duration();

        // onWork 在 DECODER 线程执行
        connect(this, &ReverseDecodeDispatcher::signalStartWorker, this, &ReverseDecodeDispatcher::onWork,
                Qt::QueuedConnection);
    }

    /// 析构 — 清空残留帧后释放全部资源
    ~ReverseDecodeDispatcher() override {
        qDebug() << "Destroy decode dispatcher " << filename.c_str();
        ReverseDecodeDispatcher::flush();
        delete audioQueue;
        delete videoQueue;
        delete m_audioDecoder;
        delete videoDecoder;
        if (packet) { av_packet_free(&packet); }
    }

    /**
     * 保证阻塞获取结果的线程尽快被唤醒, 同时请求 DecodeThread 的工作尽快停止.
     */
    PONY_THREAD_SAFE void statePause() override {
        interrupt = true;
        videoQueue->close();
        qDebug() << "Queue close.";
    }

    /// 清空音频/视频队列与解码器内的待倒序输出帧栈
    PONY_THREAD_SAFE void flush() override {
        auto freeFunc = [](AVFrame *frame) {
            if (frame) av_frame_free(&frame);
        };
        videoQueue->clear(freeFunc);
        audioQueue->clear(freeFunc);
        videoDecoder->clearFrameStack();
        m_audioDecoder->clearFrameStack();
    }


    /**
     * 保证可以阻塞地获取 Picture 和 Sample. 这个方法是线程安全的.
     */
    PONY_THREAD_SAFE void stateResume() override {
        interrupt = false;
        videoQueue->open();
        emit signalStartWorker(QPrivateSignal());
        qDebug() << "Queue stateResume.";
    }

    /**
     * 修改视频播放进度, 注意: 这个方法必须在解码线程上调用.
     * @param secs 新的视频进度(单位: 秒)
     */
    PONY_GUARD_BY(DECODER)

    void seek(qreal secs) override {
        // case 1: currently decoding, interrupt
        // case 2: not decoding, seek
        interrupt = true;
        videoDecoder->flushFFmpegBuffers();
        m_audioDecoder->flushFFmpegBuffers();
        qDebug() << "a Seek:" << secs;
        secs = fmax(secs, 0.0);
        videoDecoder->setStart(secs);
        m_audioDecoder->setStart(secs);
        int ret = av_seek_frame(fmtCtx, -1, static_cast<int64_t>((secs - interval) * AV_TIME_BASE),
                                AVSEEK_FLAG_BACKWARD);
        if (ret != 0) { qWarning() << "Error av_seek_frame:" << ffmpegErrToString(ret); }
    }

    /// 取出队首视频帧 (线程安全)
    PONY_THREAD_SAFE VideoFrameRef getPicture() override { return videoDecoder->getPicture(); }

    /// 查看队首视频帧时间戳, 不弹出 (线程安全)
    PONY_THREAD_SAFE qreal frontPicture() override { return videoDecoder->viewFront(); }

    /// 取出队首音频帧 (线程安全)
    PONY_THREAD_SAFE AudioFrame getSample() override { return m_audioDecoder->getSample(); }

    /// 查看队首音频帧时间戳, 不弹出 (尚未实现)
    PONY_THREAD_SAFE qreal frontSample() override {NOT_IMPLEMENT_YET}

    /// 启用/禁用音频输出
    PONY_GUARD_BY(DECODER)

    void setEnableAudio(bool enable) override { m_audioDecoder->setEnable(enable); }

    /// 获取音频输入格式 (待实现)
    PonyAudioFormat getAudioInputFormat() override { // TODO: IMPLEMENT LATER
        return m_audioDecoder->getInputFormat();
    }

    /// 设置音频输出格式 (待实现)
    void setAudioOutputFormat(PonyAudioFormat format) override { // TODO: IMPLEMENT LATER
        m_audioDecoder->setOutputFormat(format);
    }

    /// 当前文件是否包含视频轨
    bool hasVideo() override {
        return !isAudio;
    }

    /// 测试接口: 同步执行一次解码循环 (调试用)
    void test_onWork() override {
        onWork();
    }

    /// 按音轨序号切换音频解码器, 并重新绑定 follower / primary (需在解码空闲时调用)
    PONY_GUARD_BY(DECODER)
    void setTrack(int i) override {
        delete m_audioDecoder;
        m_audioStreamIndex = description.m_audioStreamsIndex[static_cast<size_t>(i)];
        auto stream = fmtCtx->streams[m_audioStreamIndex];
        m_audioDecoder = new ReverseDecoderImpl<Audio>(stream, audioQueue);
        if (hasVideo())
            videoDecoder->setFollower(m_audioDecoder);
        else {
            m_audioDecoder->setFollower(m_audioDecoder);
            primary = m_audioDecoder;
        }
    }

    /// 返回全部音轨的友好名称列表 (供 UI 展示)
    PONY_GUARD_BY(DECODER)
    QStringList getTracks() {
        QStringList ret;
        ret.reserve(static_cast<qsizetype>(description.m_audioStreamsIndex.size()));
        for (auto &&i: description.m_audioStreamsIndex) {
            ret.emplace_back(description.streamInfos[i].getFriendName());
        }
        return ret;
    }

    /// 按流索引切换音频解码器, 并重新绑定 follower / primary (需在解码空闲时调用)
    PONY_GUARD_BY(DECODER)
    void setAudioIndex(StreamIndex i) {
        if (i == m_audioStreamIndex) { return; }
        delete m_audioDecoder;
        m_audioStreamIndex = i;
        m_audioDecoder = new ReverseDecoderImpl<Audio>(fmtCtx->streams[m_audioStreamIndex], audioQueue);
        if (hasVideo())
            videoDecoder->setFollower(m_audioDecoder);
        else {
            m_audioDecoder->setFollower(m_audioDecoder);
            primary = m_audioDecoder;
        }
    }

private slots:

    /// 反向解码主循环 — 在 DECODER 线程执行: 分段向前解码, 倒序输出
    void onWork() {
        videoQueue->open();
        while (!interrupt) {
            // 读取 Packet 并分发给对应解码器
            int ret = av_read_frame(fmtCtx, packet);
            if (ret == 0) {
                if (static_cast<StreamIndex>(packet->stream_index) == m_videoStreamIndex) {
                    videoDecoder->accept(packet, interrupt);
                } else if (static_cast<StreamIndex>(packet->stream_index) == m_audioStreamIndex) {
                    m_audioDecoder->accept(packet, interrupt);
                }
            } else if (ret == ERROR_EOF) {
                // 读到文件末尾: 回卷到文件结尾附近, 继续解码下一段
                //std::cerr << "reverse reach eof" << std::endl;
                qDebug() << "reverse: reach eof";
                videoDecoder->flushFFmpegBuffers();
                m_audioDecoder->flushFFmpegBuffers();
                av_seek_frame(fmtCtx, -1,
                              static_cast<int64_t>((description.audioDuration - 2 * interval) * AV_TIME_BASE),
                              AVSEEK_FLAG_BACKWARD);
                videoDecoder->setStart(description.audioDuration - interval);
                m_audioDecoder->setStart(description.audioDuration - interval);
            } else {
                qWarning() << "Error av_read_frame:" << ffmpegErrToString(ret);
            }
            av_packet_unref(packet);
            // 查询主导解码器是否推进到了下一个分段
            auto next = primary->nextSegment();
            if (next > 0) {
                // 尚有更早的分段: 把起始点前移并重新 seek, 解码下一个分段
                //std::cerr << "next: " << next << std::endl;
                videoDecoder->setStart(next);
                m_audioDecoder->setStart(next);
                videoDecoder->flushFFmpegBuffers();
                m_audioDecoder->flushFFmpegBuffers();
                av_seek_frame(fmtCtx, -1,
                              static_cast<int64_t>((next - interval) * AV_TIME_BASE),
                              AVSEEK_FLAG_BACKWARD);
            } else if (next == 0) {
                // 已回卷到起始点: 以空指针标记 EOF, 结束反向解码
                //std::cerr << "reverse: finish" << std::endl;
                av_packet_unref(packet);
                videoQueue->push(nullptr);
                audioQueue->push(nullptr);
                qDebug() << "reverse: reach starting pointing";
                break;
            }
        }
        interrupt = true;
    };

signals:

    void signalStartWorker(QPrivateSignal);
};




