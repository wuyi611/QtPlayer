/**
 * @file decoders.hpp
 * @brief 解码器接口与解码上下文 — 定义正向 / 反向解码器共用的抽象接口
 *
 * 本文件包含:
 *   - IDemuxDecoder: 解码器的抽象接口, 统一描述"接收 Packet → 产出 Frame"的解码器行为
 *   - DecoderContext: 解码器的 FFmpeg 基础设施 (查找解码器 / 分配上下文 / 打开编解码器),
 *     由具体解码器实现 (forward.hpp / backward.hpp) 继承复用
 *   - Audio / Video / Common: DecoderType 的便捷别名
 */
//
// Created by ColorsWind on 2022/5/18.
//
#pragma once

#include "helper.hpp"
INCLUDE_FFMPEG_BEGIN
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
INCLUDE_FFMPEG_END
#include <QDebug>
#include "frame.hpp"
#include "twins_queue.hpp"
#include "concurrentqueue.h"
#include "audioformat.hpp"
#include <atomic>
#include <utility>

/**
 * @brief 解码器抽象接口 — 将 Packet 解码为 Frame 并提供队列访问
 *
 * 两个实现家族:
 *   - 正向解码器 (forward.hpp 的 DecoderImpl): 解码结果直接入队, 供消费线程读取
 *   - 反向解码器 (backward.hpp 的 ReverseDecoderImpl): 为倒放做分段解码,
 *     通过 setFollower / pushFrameStack / getLastPts / setStart / nextSegment 协作推进
 *
 * 正向家族只用到前一半接口 (accept / getPicture / getSample / viewFront / skip /
 * duration / setEnable / getInputFormat / setOutputFormat / flushFFmpegBuffers);
 * 反向家族额外实现后一半的分段推进接口.
 */
class IDemuxDecoder {

public:
    /**
     * 解码器类型 — 作为模板参数选择具体实现 (见 forward.hpp / backward.hpp)
     */
    enum class DecoderType {
        Audio,  ///< 音频解码器
        Video,  ///< 视频解码器
        Common, ///< 未指定
    };
    /**
     * 接收一个包
     * @param pkt
     * @return 如果还需要接收下一个 packet 返回 true, 否则返回 false
     */
    virtual bool accept(AVPacket *pkt, std::atomic<bool> &interrupt) = 0;

    /**
     * 清空 FFmpeg 内部缓冲区
     */
    virtual void flushFFmpegBuffers() = 0;


    /**
     * 获取视频帧并从队列中删除, 仅当当前解码器是视频解码器时有效
     * @param b 是否阻塞
     * @return 视频帧, 请用 isValid 判断是否有效
     */
    virtual VideoFrameRef getPicture() = 0;

    /**
    * 获取音频帧并从队列中删除, 仅当当前解码器是音频解码器时有效
    * @param b 是否阻塞
    * @return 音频帧, 请用 isValid 判断是否有效
    */
    virtual AudioFrame getSample() = 0;

    /**
     * 获取队首帧的PTS, 若不存在, 返回NaN
     * @return 队首帧的PTS
     */
    virtual qreal viewFront() = 0;

    /**
     * 线性扫描移除满足条件的帧, 当发现帧不满足条件时, 结束扫描
     * @param predicate 条件
     * @return 移除帧的个数
     */
    virtual int skip(const std::function<bool(qreal)> &predicate) = 0;

    /**
     * 获取流的长度
     * @return
     */
    virtual double duration() = 0;


    /// 启用/禁用解码输出 (禁用时的具体行为由实现定义, 如丢弃包或停止入队)
    virtual void setEnable(bool b) = 0;

    virtual ~IDemuxDecoder() = default;

    /// (反向专用) 设置跟随解码器 — follower 追上 leader 时触发整段帧倒序输出
    virtual void setFollower(IDemuxDecoder* follower) {NOT_IMPLEMENT_YET}

    /// (反向专用) 把帧栈中的帧倒序压入输出队列
    virtual void pushFrameStack() {}

    /// (反向专用) 当前已解码帧的最新 PTS (反向推进的分段边界参考)
    virtual qreal getLastPts() {
        NOT_IMPLEMENT_YET
    }

    /// (反向专用) 清空帧栈 (丢弃待倒序输出的已缓存帧)
    virtual void clearFrameStack() {}

    /// (反向专用) 设置分段解码的起点时间
    virtual void setStart(qreal secs) {}

    /// (反向专用) 查询下一个待解码分段的起点; 返回 0 表示已回卷到文件起点
    virtual qreal nextSegment() {
        NOT_IMPLEMENT_YET
    }

    /// 获取解码器输入格式 (音频: 源文件的采样格式)
    virtual PonyAudioFormat getInputFormat() = 0;

    /// 设置解码器输出格式 (音频: 重采样目标格式)
    virtual void setOutputFormat(const PonyAudioFormat& format) = 0;
};

/**
 * @brief 解码上下文 — 封装单个媒体流的 FFmpeg 解码器初始化与释放
 *
 * 负责: 按 codec_id 查找解码器 → 分配 AVCodecContext → 拷贝流参数 → 打开解码器
 * → 分配帧缓冲. 供 DecoderImpl (正向) 与 ReverseDecoderImpl (反向) 继承复用.
 * RAII: 析构时自动释放全部 FFmpeg 资源.
 */
class DecoderContext {
public:
    const AVCodec *codec = nullptr;     ///< 查找到的解码器 (FFmpeg 5.0+ 为 const 不透明类型, 全局单例, 不归本类释放)
    AVStream *stream = nullptr;         ///< 关联的媒体流 (属于 fmtCtx, 不归本类释放)
    AVCodecContext *codecCtx = nullptr; ///< 解码上下文 (本类负责释放)
    AVFrame *frameBuf = nullptr;        ///< 解码帧缓冲 (本类负责释放; 每解出一帧后需重新分配)
    /**
     * @brief 初始化解码器 — 从媒体流参数创建并打开编解码器
     * @param vs 目标媒体流
     * @throw std::runtime_error 解码器查找 / 上下文分配 / 参数拷贝 / 打开 / 帧缓冲分配任一失败时抛出
     */
    DecoderContext(AVStream *vs): stream(vs) {
        const AVCodecParameters *par = stream->codecpar;
        // ① 按流中记录的 codec_id 查找对应解码器 (FFmpeg 5.0+ 返回 const AVCodec*, 无需 const_cast)
        if (!(codec = avcodec_find_decoder(par->codec_id))) {
            throw std::runtime_error("Cannot find valid decoder.");
        }
        // ② 分配解码上下文
        if (!(codecCtx = avcodec_alloc_context3(codec))) {
            throw std::runtime_error("Cannot alloc codec context.");
        }
        // ③ 把流的编解码参数拷贝进解码上下文
        if (avcodec_parameters_to_context(codecCtx, par) < 0) {
            throw std::runtime_error("Cannot initialize codec context.");
        }
        // ④ 现代推荐配置:
        //    thread_count = 0  → 自动帧级多线程解码 (高分辨率视频吞吐提升);
        //    pkt_timebase     → 时间基对齐流, 保证解码器产出的 pts/dts 换算准确
        codecCtx->thread_count = 0;
        codecCtx->pkt_timebase  = stream->time_base;
        // ⑤ 打开解码器 (可传 AVDictionary 传入 threads / lowres 等编解码器选项)
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            throw std::runtime_error("Cannot open codec.");
        }
        // ⑥ 分配一个帧缓冲, 供 avcodec_receive_frame 填充解码结果
        if (!(frameBuf = av_frame_alloc())) {
            throw std::runtime_error("Cannot alloc frame buf.");
        }
    }

    /// 析构 — 释放帧缓冲与解码上下文
    ~DecoderContext() {
        if (frameBuf) { av_frame_free(&frameBuf); }
        // 现代: avcodec_free_context 内部已关闭解码器并释放上下文;
        // avcodec_close 自 FFmpeg 5.0 起废弃, 不应再单独调用
        if (codecCtx) { avcodec_free_context(&codecCtx); }
    }

};


/// DecoderType 便捷别名: 音频解码器 (供模板特化使用)
constexpr auto Audio  =  IDemuxDecoder::DecoderType::Audio;
/// DecoderType 便捷别名: 视频解码器 (供模板特化使用)
constexpr auto Video  =  IDemuxDecoder::DecoderType::Video;
/// DecoderType 便捷别名: 通用 (未特化)
constexpr auto Common =  IDemuxDecoder::DecoderType::Common;
