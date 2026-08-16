/**
 * @file forward.hpp
 * @brief 正向解码器实现 (DecoderImpl) — 正常播放方向的 FFmpeg 解码封装
 *
 * 结构:
 *   - DecoderImpl<Common>: 通用解码逻辑 (送包、取帧入队、时长 / 帧浏览 / 跳过 / 缓冲清空)
 *   - DecoderImpl<Audio>: 音频特化 — 增加重采样 (SwrContext), 输出指定格式的采样数据
 *   - DecoderImpl<Video>: 视频特化 — 处理第一帧为封面的特殊情况, 输出 YUV 帧
 *
 * 与调度器的协作: DecodeDispatcher::onWork 循环调用 accept(pkt) 送入 Packet;
 * 解码产出的 AVFrame 通过 frameQueue (TwinsBlockQueue) 提供给消费线程 getSample / getPicture.
 */
//
// Created by ColorsWind on 2022/5/1.
//
#pragma once

#include "decoders.hpp"

/**
 * @brief 正向解码器通用实现 — 完成 FFmpeg "送包-取帧" 的解码协议
 *
 * 借助基类 DecoderContext 把 AVStream 初始化为可用的解码器, 并把解码产出的
 * AVFrame 写入 frameQueue. 通用部分承载公共逻辑, 音频/视频的差异由底部特化类补齐.
 *
 * @tparam type 解码器类型 (Audio / Video)
 */
template<IDemuxDecoder::DecoderType type>
class DecoderImpl : public DecoderContext, public IDemuxDecoder {
protected:
    /// 解码结果写入的联动队列 (由调度器创建)
    TwinsBlockQueue<AVFrame *> *frameQueue;
public:
    /// 以某个媒体流初始化解码器并绑定输出队列
    DecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue)
            : DecoderContext(vs), frameQueue(queue) {}

    /// 流时长 (秒) = 流的 duration × time_base
    PONY_THREAD_SAFE double duration() override {
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    /**
     * @brief 送入一个 Packet, 并取出本次能解出的全部帧 (在 DECODER 线程调用)
     * @param pkt 待解码的压缩包
     * @param interrupt 中断标志; 置位时立即停止取帧
     * @return true 表示解码器仍需下一个 Packet (如 EAGAIN); false 表示本包处理完毕
     *
     * FFmpeg 推拉模型: avcodec_send_packet 送包, 循环 avcodec_receive_frame 取帧.
     *   - 取到一帧 (ret >= 0): 压入 frameQueue; 入队失败说明队列已关闭, 清队并终止
     *   - EAGAIN: 解码器输出已取空, 需要更多输入
     *   - EOF: 码流解码完毕, 压入 nullptr 作为 EOF 哨兵
     */
    PONY_GUARD_BY(DECODER) bool accept(AVPacket *pkt, std::atomic<bool> &interrupt) override {
        // 队列被禁用时直接"吞掉"该包, 不进入解码.
        // 返回 true 让调度器以为"解码器还需要更多输入", 从而继续读下一个包
        if (!frameQueue->isEnable())
            return true;
        // ① 把压缩包送入解码器.
        // avcodec_send_packet: 将压缩数据(ES流)交给解码器内部缓冲, 0 表示已接受;
        // 负数通常意味着 packet 非法或码流数据损坏
        int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            qWarning() << "Error avcodec_send_packet:" << ffmpegErrToString(ret);
            return false;
        }
        // ② 反复取帧, 直到取空或被打断.
        // 关键认知: 一个 Packet 可能解出 0 ~ N 帧 (受 B帧重排/编码器缓冲影响),
        // 所以必须用 while 循环取到 EAGAIN 为止, 不能假设"一个包 = 一帧"
        while(ret >= 0 && !interrupt) {
            // avcodec_receive_frame: 从解码器取出一个已解码的原始帧到 frameBuf
            ret = avcodec_receive_frame(codecCtx, frameBuf);
            if (ret >= 0) {
                // 取到一帧: 入队.
                // 入队失败说明队列已被关闭 (statePause 调用 close), 此时清空残留帧并终止,
                // 避免队列已关、帧无人消费导致的内存泄漏
                if(!frameQueue->push(frameBuf)) {
                    frameQueue->clear([](AVFrame *frame) { av_frame_free(&frame); });
                    av_frame_unref(frameBuf);
                    return false;
                }
                // 帧已移交队列 (队列负责释放), 这里重新分配一个空帧缓冲,
                // 否则下一次 receive_frame 会覆盖同一个指针导致悬垂引用
                frameBuf = av_frame_alloc();
            } else if (ret == AVERROR(EAGAIN)) {
                // EAGAIN = 解码器内部缓冲已取空, 暂时没有可输出的帧,
                // 返回 true 通知调度器"还需要下一个 Packet 才能继续"
                return true;
            } else if (ret == ERROR_EOF) {
                // 码流解码完毕: 向消费队列压入 nullptr 作为 EOF 哨兵,
                // 消费端 getSample/getPicture 见到空帧即知播放结束
                frameQueue->push(nullptr);
                return false;
            } else {
                // 其它负数: 真正的解码错误 (数据损坏等)
                frameQueue->push(nullptr);
                qWarning() << "Error avcodec_receive_frame:" << ffmpegErrToString(ret);
                return false;
            }
        }
        return false;
    }

    /// 以下通用接口由 Audio / Video 特化实现, 此处仅占位
    PONY_THREAD_SAFE VideoFrameRef getPicture() override { NOT_IMPLEMENT_YET }

    PONY_THREAD_SAFE AudioFrame getSample() override { NOT_IMPLEMENT_YET }

    PonyAudioFormat getInputFormat() override { NOT_IMPLEMENT_YET }

    void setOutputFormat(const PonyAudioFormat &format) override { NOT_IMPLEMENT_YET }


    /// 查看队首帧的 PTS (秒); 队列空或遇到 EOF (nullptr) 时返回 NaN
    PONY_THREAD_SAFE qreal viewFront() override {
        // viewFront<T> 由 TwinsBlockQueue 提供: 在锁保护下"只看不移除"队首元素.
        // 回调把解码帧原始 pts (以 time_base 为单位) 换算成秒
        return frameQueue->viewFront<qreal>([this](AVFrame * frame) {
            if (frame) {
                // FFmpeg 中 frame->pts 是整数时间戳, 需乘以 stream->time_base 的倒比得到秒.
                // av_q2d(rational) 把 AVRational 换算成 double 倍率
                return static_cast<qreal>(frame->pts) * av_q2d(stream->time_base);
            } else {
                // 队首是 nullptr = EOF 哨兵, 用 NaN 标记"无有效帧"
                return std::numeric_limits<qreal>::quiet_NaN();
            }
        });
    }

    /// 线性跳过 PTS 满足 predicate 的队首帧并释放它们; 返回实际跳过的帧数
    PONY_THREAD_SAFE int skip(const std::function<bool(qreal)> &predicate) override {
        // 第一个 lambda 是"判定": 把帧 pts 转秒后交给 predicate, 为 true 则跳过
        // (nullptr 哨兵不参与判定, 直接视为不满足); 第二个 lambda 是"释放":
        // 跳过的帧要 av_frame_free 归还给 FFmpeg, 防止内存泄漏.
        // skip 语义: 线性扫描直到遇到第一个不满足条件的帧即停止 (用于 seek 时快速丢弃旧帧)
        return frameQueue->skip([this, predicate](AVFrame *frame){
            return frame && predicate(static_cast<qreal>(frame->pts) * av_q2d(stream->time_base));
        }, [](AVFrame *frame) { av_frame_free(&frame); });
    }

    /// 启用/禁用输出队列 (禁用时 accept 会直接丢弃 Packet)
    PONY_THREAD_SAFE void setEnable(bool b) override {
        frameQueue->setEnable(b);
    }

    /// 清空 FFmpeg 解码器内部缓冲 (seek 后调用, 丢弃解码器残留的历史帧)
    PONY_GUARD_BY(DECODER) void flushFFmpegBuffers() override {
        avcodec_flush_buffers(codecCtx);
    }

};

/**
 * @brief 音频解码器特化 — 在通用解码基础上增加重采样
 *
 * 解码产出的原始采样 (codecCtx 的格式) 经 swr_convert 重采样为 targetFmt 指定的
 * 目标格式 (默认 Int16 / 44100Hz / 双声道), 供音频后端直接播放.
 */
template<> class DecoderImpl<Audio>: public DecoderImpl<Common> {
    SwrContext *swrCtx = nullptr;        ///< 重采样上下文 (由 setOutputFormat 创建, 析构释放)
    uint8_t *audioOutBuf = nullptr;      ///< 重采样输出缓冲 (每次 getSample 复用; 返回的 AudioFrame 指向它)
    AVFrame * sampleFrameBuf = nullptr;  ///< (预留) 采样帧缓冲, 当前未使用
    PonyAudioFormat targetFmt = PonyAudioFormat(PonyPlayer::Int16, 44100, 2); ///< 目标输出格式 (默认 Int16/44100Hz/双声道)

public:
    /// 构造 — 分配重采样输出缓冲与采样帧
    DecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue) : DecoderImpl<Common>(vs, queue) {
        if (!(audioOutBuf = static_cast<uint8_t *>(av_malloc(2 * MAX_AUDIO_FRAME_SIZE)))) {
            throw std::runtime_error("Cannot alloc audioOutBuf");
        }
        sampleFrameBuf = av_frame_alloc();
    }

    /// 析构 — 释放重采样器 / 输出缓冲 / 采样帧
    virtual ~DecoderImpl() override {
        if (sampleFrameBuf) { av_frame_free(&sampleFrameBuf); }
        if (audioOutBuf) { av_freep(&audioOutBuf); }
        if (swrCtx) { swr_free(&swrCtx); }
    }


    /// 取出队首音频帧并重采样为目标格式 (线程安全)
    PONY_THREAD_SAFE AudioFrame getSample() override {
        // 队列被禁用时属非法状态
        if (!frameQueue->isEnable()) {
            ILLEGAL_STATE("forward: getSample is disabled");
        }

        // remove(true): 阻塞弹出队首解码帧; 若队首是 nullptr (EOF 哨兵) 则返回空 AudioFrame
        AVFrame *frame = frameQueue->remove(true);
        if (!frame) { return {}; }
        // 记录该帧起始时刻的 PTS (秒), 用于上层音频同步
        double pts = static_cast<double>(frame->pts) * av_q2d(stream->time_base);
        // swr_convert: 把解码器原生采样格式 (采样率/声道/位深) 转换成 targetFmt 目标格式.
        // 解码出的原始 PCM 存入 audioOutBuf; 返回值 len 是重采样后的样本数
        int len = swr_convert(swrCtx, &audioOutBuf, 2 * MAX_AUDIO_FRAME_SIZE,
                              const_cast<const uint8_t **>(frame->data), frame->nb_samples);

        // 由 len 个样本 × 目标声道数 × 目标位深, 换算输出 PCM 的字节数
        int out_size = av_samples_get_buffer_size(nullptr, targetFmt.getChannelCount(),
                                                  len,
                                                  targetFmt.getSampleFormatForFFmpeg(),
                                                  1);
        av_frame_free(&frame);
        // 注意: 返回的 AudioFrame 指向内部复用的 audioOutBuf,
        // 它是一次性缓冲, 下一次 getSample 会覆盖内容, 消费端必须尽快使用
        return {reinterpret_cast<std::byte *>(audioOutBuf), out_size, pts};
    }

    /// 获取解码器输入格式 (源文件的采样格式 / 采样率 / 声道数)
    PonyAudioFormat getInputFormat() override {
        return {PonyPlayer::valueOf(codecCtx->sample_fmt), codecCtx->sample_rate, codecCtx->channels};
    }

    /// 设置输出格式 — 按新目标重建重采样器, 之后 getSample 输出该格式的采样
    void setOutputFormat(const PonyAudioFormat& format) override {
        targetFmt = format;
        // 销毁旧重采样器 (目标格式已变, 旧 swrCtx 参数不适用)
        if (swrCtx) { swr_free(&swrCtx); }
        // swr_alloc_set_opts(输出参数..., 输入参数..., 0, nullptr):
        //   输出: 目标声道布局 / 目标采样格式 / 目标采样率 (来自外部传入的 format)
        //   输入: 解码器自身的声道布局 / 原始采样格式 / 原始采样率 (codecCtx 只读属性)
        // 最后一个参数 (0, nullptr) 为日志级别与日志上下文, 传默认即可
        this->swrCtx = swr_alloc_set_opts(swrCtx, av_get_default_channel_layout(format.getChannelCount()),
                                          format.getSampleFormatForFFmpeg(), format.getSampleRate(),
                                          static_cast<int64_t>(codecCtx->channel_layout), codecCtx->sample_fmt,
                                          codecCtx->sample_rate, 0, nullptr);

        // swr_init 完成参数检查与内部表初始化; 失败说明输出/输入参数组合不合法
        if (!swrCtx || swr_init(swrCtx) < 0) {
            throw std::runtime_error("Cannot initialize swrCtx");
        }
    }

};

/**
 * @brief 视频解码器特化 — 处理"第一帧为封面"的常见情况
 *
 * 部分媒体文件的第一帧 PTS < 0, 实际是专辑封面而非播放画面.
 * 遇到时将其保存为 stillVideoFrame, 之后 getPicture 恒返回该静态封面帧,
 * 直到正常视频帧出现.
 */
template<> class DecoderImpl<Video>: public DecoderImpl<Common> {
private:
    /**
     * 如果视频的第一帧 pts < 0, 则说明第一帧为封面. 保存下来.
     */
    std::atomic<AVFrame *> stillVideoFrame = nullptr;
public:
    /// 以视频流初始化解码器并绑定输出队列
    DecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue) : DecoderImpl<Common>(vs, queue) {}


    /// 取出队首视频帧 (线程安全); 已缓存封面时直接返回封面帧
    VideoFrameRef getPicture() override {
        // 已保存封面: 直接返回静态封面帧.
        // 此时队列里真正的视频帧照常推进, 但 getPicture 优先把封面帧交给上层,
        // 用于"视频刚开始/音频文件"时显示专辑封面. PTS 用 -1 标记"非时间轴帧"
        if (stillVideoFrame != nullptr) { return {stillVideoFrame, true, -1}; }
        // 弹出队首解码帧; 空指针 (EOF 哨兵) 返回空引用
        AVFrame *frame = frameQueue->remove(true);
        if (!frame) { return {}; }
        // 判断第一帧是否封面: 真实播放画面 PTS ≥ 0, 而部分媒体 (如带封面的 mp3)
        // 第一帧是附加图片, PTS < 0. 遇到时缓存该帧, 此后 getPicture 恒返回它作为封面
        if (frame->pts < 0) {
            stillVideoFrame = frame;
            return {frame, true, -9};  // -9 是约定的"封面帧"标记, 供上层识别
        } else {
            // 正常视频帧: 换算成秒返回
            double pts = static_cast<double>(frame->pts) * av_q2d(stream->time_base);
            return {frame, true, pts};
        }
    }


    /// 析构 — 释放缓存的封面帧
    ~DecoderImpl() override {
        auto *frame = stillVideoFrame.load();
        if (frame) { av_frame_free(&frame); }
    }


};
