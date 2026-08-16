/**
 * @file backward.hpp
 * @brief 反向解码器实现 (ReverseDecoderImpl) — 倒放方向的 FFmpeg 解码封装
 *
 * FFmpeg 只支持正向解码, 倒放通过"分段解码 + 内存倒序"实现:
 *   1. 每次解码 [from-interval, from) 时间段内的帧, 暂存进 frameStack
 *   2. 当 follower (跟随解码器) 的 PTS 追到 from 时, pushFrameStack 把整段帧【倒序】压入队列
 *   3. 上一段消费完毕后, 把 from 前移一个 interval, 重新 seek 到更早位置, 解码下一段
 *
 * 与调度器的协作: ReverseDecodeDispatcher::onWork 循环调用 accept(pkt) 送入 Packet,
 * 并通过 primary / m_follower 的绑定关系协调音视频解码器的分段推进.
 */
//
// Created by ColorsWind on 2022/5/18.
//
#pragma once

/**
 * @brief 反向解码器通用实现 — 为倒放做分段解码与倒序输出
 *
 * 与正向 DecoderImpl 直接入队不同, 反向解码把帧先暂存再倒序输出:
 *   - accept 时, 把落在当前分段 [from-interval, from) 内的帧存进 frameStack
 *   - 当 follower 的 lastPts 追到 from (双方都解到了本段末尾) 时,
 *     pushFrameStack 从栈尾弹出帧, 实现时间轴倒放
 *
 * @tparam type 解码器类型 (Audio / Video)
 */
template<IDemuxDecoder::DecoderType type>
class ReverseDecoderImpl : public DecoderContext, public IDemuxDecoder {
protected:
    const qreal interval = 5.0;              ///< 每个分段时长 (秒); 也是 seek 回卷的提前量
    TwinsBlockQueue<AVFrame *> *frameQueue;  ///< 解码结果输出队列 (由调度器创建, 倒序后入队)
    std::vector<AVFrame*> *frameStack;       ///< 本分段暂存的帧栈; 倒序输出时从尾部弹出
    IDemuxDecoder* m_follower{};             ///< 跟随解码器; 它追上 leader 时触发整段倒序输出
    qreal lastPts{-1.0};                     ///< 最近解码帧的 PTS (秒); 用于判断 follower 是否追上
    qreal from;                              ///< 当前分段的结束时刻; 本段解码范围是 [from-interval, from)
    qreal next{-1.0};                        ///< 下一个待解码分段的起点; 由 nextSegment() 读取并复位

public:
    /// 以媒体流初始化解码器并绑定输出队列; 起点 from 初始化为文件末尾
    ReverseDecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue) :
            DecoderContext(vs), frameQueue(queue), frameStack(new std::vector<AVFrame*>) {
        // 倒放从文件末尾开始, 因此 from 初始化为流的时长
        from = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    /// 绑定跟随解码器; follower 追上 leader 时触发 pushFrameStack 倒序输出
    void setFollower(IDemuxDecoder* follower) override {
        m_follower = follower;
    }

    /// 把帧栈中的帧【倒序】压入输出队列: 从栈尾弹出, 时间上最晚的帧先入队
    void pushFrameStack() override {
        while (!frameStack->empty()) {
            frameQueue->push(frameStack->back());
            frameStack->pop_back();
        }
    }

    /// 返回最近解码帧的 PTS; 调度器据此判断 follower 是否追上了 leader
    qreal getLastPts() override {
        return lastPts;
    }

    /// 清空帧栈: 释放其中所有暂存的 AVFrame (seek / flush 时丢弃未倒序输出的帧)
    void clearFrameStack() override {
        if (frameStack) {
            for (auto frame: *frameStack) {
                if (frame) av_frame_free(&frame);
            }
            frameStack->clear();
        }
    }

    /// 设置分段解码的起点 (秒); 同时重置 lastPts 并清空旧帧栈
    void setStart(qreal secs) override {
        from = secs;
        lastPts = -1.0;
        clearFrameStack();
    }

    /// 查询下一个待解码分段的起点; 返回 0 表示已回卷到文件起点
    /// 注意: 读取后将 next 复位为 -1, 保证同一分段边界只报告一次
    qreal nextSegment() override {
        auto res = next;
        next = -1.0;
        return res;
    }

    /// 流时长 (秒) = 流的 duration × time_base
    double duration() override {
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    /// 析构 — 释放帧栈中所有暂存帧及帧栈本身
    ~ReverseDecoderImpl() override {
        if (frameStack) {
            for (auto frame: *frameStack) {
                if (frame) av_frame_free(&frame);
            }
            delete frameStack;
        }
    }

    /**
     * @brief 送入一个 Packet, 解码后按帧的 PTS 决定去向 (在 DECODER 线程调用)
     * @param pkt 待解码的压缩包
     * @param interrupt 中断标志; 置位时立即停止取帧
     * @return true 表示解码器仍需下一个 Packet; false 表示本包处理完毕
     *
     * 反向分段解码核心逻辑: 解码出的每一帧按 PTS 落在哪个区间决定去向
     *   - PTS < from-interval  : 属于更早的分段, 直接丢弃 (当前分段不覆盖)
     *   - PTS >= from          : 已超过当前分段末尾, 说明本段解完; 若 follower
     *                           也追到了 from, 则把整段帧倒序压入队列
     *   - 其余 (分段范围内)    : 暂存进 frameStack, 等待倒序输出
     */
    bool accept(AVPacket *pkt, std::atomic<bool> &interrupt) override {
        // ① 送包入解码器 (与正向 DecoderImpl 一致)
        int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            qWarning() << "Error avcodec_send_packet:" << ffmpegErrToString(ret);
            return false;
        }
        // ② 反复取帧, 按 PTS 判断每帧归属
        while(ret >= 0 && !interrupt) {
            ret = avcodec_receive_frame(codecCtx, frameBuf);
            if (ret >= 0) {
                lastPts = av_q2d(stream->time_base) * static_cast<double>(frameBuf->pts);
                // 情形一: 帧早于当前分段起点, 属于上一个 (更早的) 分段, 丢弃
                if (lastPts < from-interval) {
                    av_frame_unref(frameBuf);
                    continue;
                }
                // 情形二: 帧达到或超过分段末尾 -> 当前分段解码结束
                else if (lastPts >= from){
                    av_frame_unref(frameBuf);
                    // 只有 leader 有权决定跳转: 必须等 follower 也解到 from,
                    // 双方帧栈都攒满了本段, 才一起倒序输出, 保证音画同步
                    if (m_follower && m_follower->getLastPts() >= from) {
                        // 倒序输出整段: 先出自己再出 follower, 时间上最晚的帧先入队
                        pushFrameStack();
                        m_follower->pushFrameStack();
                        // 起点前移一个分段, 准备解码更早的一段
                        if (from < interval)
                            from = 0;       // 已到文件头部, 下一段从 0 开始
                        else
                            from -= interval;
                        next = from;         // 记录给调度器: 需要重新 seek 到更早位置
                        return true;
                    }
                }
                // 情形三: 帧落在 [from-interval, from) 分段内 -> 暂存待倒序
                else {
                    frameStack->push_back(frameBuf);
                    frameBuf = av_frame_alloc();
                }
            } else if (ret == AVERROR(EAGAIN)) {
                // 暂无输出帧, 需要下一个 Packet
                return true;
            } else if (ret == ERROR_EOF) {
                // 本段码流解码完毕 (EOF 由调度器处理回卷, 这里返回 false)
                return false;
            } else {
                qWarning() << "Error avcodec_receive_frame:" << ffmpegErrToString(ret);
                return false;
            }
        }
        return false;
    }

    /// 清空 FFmpeg 解码器内部缓冲 (seek 回卷后调用, 丢弃解码器残留的历史帧)
    void flushFFmpegBuffers() override {
        avcodec_flush_buffers(codecCtx);
    }

    /// 以下通用接口由 Video / Audio 特化实现, 此处仅占位
    VideoFrameRef getPicture() override {
        NOT_IMPLEMENT_YET
    }

    AudioFrame getSample() override {
        NOT_IMPLEMENT_YET
    }

    PONY_THREAD_SAFE void setEnable(bool b) override {
        NOT_IMPLEMENT_YET
    }

    qreal viewFront() override {
        NOT_IMPLEMENT_YET
    }

    PonyAudioFormat getInputFormat() override { NOT_IMPLEMENT_YET }

    void setOutputFormat(const PonyAudioFormat &format) override { NOT_IMPLEMENT_YET }

    int skip(const std::function<bool(qreal)> &predicate) override { NOT_IMPLEMENT_YET }
};

/**
 * @brief 反向视频解码器特化 — 提供队首视频帧的取出 / 浏览 / 跳过
 *
 * 帧的获取逻辑与正向一致 (从 frameQueue 取), 但入队顺序是倒序的,
 * 因此消费端拿到的视频帧时间戳是递减的 (倒放效果).
 */
template<>
class ReverseDecoderImpl<Video>: public ReverseDecoderImpl<Common> {
public:
    /// 以视频流初始化解码器并绑定输出队列
    ReverseDecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue)
            : ReverseDecoderImpl<Common>(vs, queue) {}

    /// 取出队首视频帧 (线程安全); 空指针 (EOF 哨兵) 返回空引用
    VideoFrameRef getPicture() override {
        AVFrame *frame = frameQueue->remove(true);
        if (!frame) {
            qDebug() << "getPicture: get EOF";
            return {};
        }
        //        m_lifeCycleManager->pop();
        // 换算成秒: 注意倒放时该 PTS 是递减的
        double pts = static_cast<double>(frame->pts) * av_q2d(stream->time_base);
        return {frame, true, pts};
    }

    /// 查看队首视频帧的 PTS (秒), 不弹出; 空或 EOF 返回 NaN
    qreal viewFront() override {
        // 回调把帧 pts 换算成秒; nullptr 是 EOF 哨兵, 用 NaN 标记无有效帧
        return frameQueue->viewFront<qreal>([this](AVFrame * frame) {
            if (frame) {
                return static_cast<qreal>(frame->pts) * av_q2d(stream->time_base);
            } else {
                return std::numeric_limits<qreal>::quiet_NaN();
            }
        });
    }

    /// 线性跳过满足 predicate 的队首帧并释放; 返回实际跳过的帧数
    int skip(const std::function<bool(qreal)> &predicate) override {
        // 判定 lambda: 帧 pts 转秒后交给 predicate, 为 true 则跳过; 释放 lambda: av_frame_free
        return frameQueue->skip([this, predicate](AVFrame *frame){
            return frame && predicate(static_cast<qreal>(frame->pts) * av_q2d(stream->time_base));
        }, [](AVFrame *frame) { av_frame_free(&frame); });
    }

};

/**
 * @brief 反向音频解码器特化 — 在分段倒序基础上增加重采样与采样数据反转
 *
 * 除了像正向那样把解码帧重采样到目标格式, 还要调用 reverseSample 把
 * 每个音频块内部的 PCM 采样序列反转: 因为帧本身已是倒序到达, 只有
 * 块内采样也倒过来, 声音才是真正的倒放 (否则是"倒序帧的正向声音").
 */
template<> class ReverseDecoderImpl<Audio>: public ReverseDecoderImpl<Common> {
    SwrContext *swrCtx = nullptr;        ///< 重采样上下文 (由 setOutputFormat 创建, 析构释放)
    uint8_t *audioOutBuf = nullptr;      ///< 重采样输出缓冲 (每次 getSample 复用; 返回的 AudioFrame 指向它)
    AVFrame * sampleFrameBuf = nullptr;  ///< (预留) 采样帧缓冲, 当前未使用
    PonyAudioFormat targetFmt = PonyAudioFormat(PonyPlayer::Int16, 44100, 2); ///< 目标输出格式 (默认 Int16/44100Hz/双声道)
public:
    /// 构造 — 分配重采样输出缓冲与采样帧
    ReverseDecoderImpl(AVStream *vs, TwinsBlockQueue<AVFrame *> *queue)
            : ReverseDecoderImpl<Common>(vs, queue) {
        if (!(audioOutBuf = static_cast<uint8_t *>(av_malloc(2 * MAX_AUDIO_FRAME_SIZE)))) {
            throw std::runtime_error("Cannot alloc audioOutBuf");
        }
        sampleFrameBuf = av_frame_alloc();
    }

    /// 析构 — 释放重采样器 / 输出缓冲 / 采样帧
    ~ReverseDecoderImpl() override {
        if (sampleFrameBuf) { av_frame_free(&sampleFrameBuf); }
        if (audioOutBuf) { av_freep(&audioOutBuf); }
        if (swrCtx) { swr_free(&swrCtx); }
    }

    /// 就地反转一段 PCM 采样: 对称位置交换每个采样点 (实现采样级倒放)
    void reverseSample(uint8_t *samples, int len) {
        int sampleSize = targetFmt.getBytesPerSampleChannels();
        int left = 0, right = len-sampleSize;
        while (left < right) {
            for (int i = 0; i < sampleSize; i++)
                std::swap(samples[left+i], samples[right+i]);
            left += sampleSize;
            right -= sampleSize;
        }
    }

    /// 取出队首音频帧, 重采样并反转采样序列 (线程安全)
    PONY_THREAD_SAFE AudioFrame getSample() override {
        // 队列被禁用时属非法状态
        if (!frameQueue->isEnable()) {
            ILLEGAL_STATE("forward: getSample is disabled");
        }
        auto *frame = std::forward<AVFrame *>(frameQueue->remove(true)); // suppress warning
        if (!frame) {
            qDebug() << "getSample: get EOF";
            return {};
        }
        // 重采样到目标格式, 写入 audioOutBuf; len 为重采样后的样本数
        int len = swr_convert(swrCtx, &audioOutBuf, 2 * MAX_AUDIO_FRAME_SIZE,
                              const_cast<const uint8_t **>(frame->data), frame->nb_samples);

        // 由样本数 × 声道数 × 位深换算输出 PCM 的字节数
        int out_size = av_samples_get_buffer_size(nullptr, targetFmt.getChannelCount(),
                                                  len,
                                                  targetFmt.getSampleFormatForFFmpeg(),
                                                  1);
        // PTS 计算: 帧起点时间 + 该块时长 (秒); 倒放块被反转, 时间戳对应块末尾
        double pts = static_cast<double>(frame->pts) * av_q2d(stream->time_base) +
                    static_cast<double>(len)/44100;
        reverseSample(audioOutBuf, out_size);
        // 块内采样反转后再按目标采样率补偿一次时长偏移
        pts += static_cast<double>(len)/targetFmt.getSampleRate();
        av_frame_free(&frame);
        // 返回的 AudioFrame 指向复用的 audioOutBuf, 下一次 getSample 会覆盖
        return {reinterpret_cast<std::byte *>(audioOutBuf), out_size, pts};
    }

    /// 获取解码器输入格式 (源文件的采样格式 / 采样率 / 声道数)
    PonyAudioFormat getInputFormat() override {
        return {PonyPlayer::valueOf(codecCtx->sample_fmt), codecCtx->sample_rate, codecCtx->channels};
    }

    /// 设置输出格式 — 按新目标重建重采样器
    void setOutputFormat(const PonyAudioFormat& format) override {
        targetFmt = format;
        // 销毁旧重采样器 (目标格式已变, 旧 swrCtx 参数不适用)
        if (swrCtx) { swr_free(&swrCtx); }
        // swr_alloc_set_opts(输出: 目标声道/格式/采样率, 输入: 解码器声道/格式/采样率, ...)
        this->swrCtx = swr_alloc_set_opts(swrCtx, av_get_default_channel_layout(format.getChannelCount()),
                                          format.getSampleFormatForFFmpeg(), format.getSampleRate(),
                                          static_cast<int64_t>(codecCtx->channel_layout), codecCtx->sample_fmt,
                                          codecCtx->sample_rate, 0, nullptr);

        // swr_init 完成参数检查与内部表初始化
        if (!swrCtx || swr_init(swrCtx) < 0) {
            throw std::runtime_error("Cannot initialize swrCtx");
        }
    }

};

