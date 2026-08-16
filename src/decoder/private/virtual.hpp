/**
 * @file virtual.hpp
 * @brief 虚拟视频解码器 — 为纯音频文件提供一个"空"的视频轨, 保持视频管线正常运行
 */
//
// Created by ColorsWind on 2022/6/1.
//

#pragma once


/**
 * @brief 虚拟视频播放, 用于纯音频文件 (如 mp3 / wav).
 *
 * 播放纯音频文件时, 上层视频渲染管线仍然需要持续"取帧"来推进; 本类不进行任何
 * 真实解码, 而是模拟一条时长等于音频长度的视频轨:
 *   - getPicture 返回"有效但无图像数据"的帧 (frame=nullptr, isValid=true), 令视频
 *     线程不会阻塞等待真实视频帧;
 *   - 其余接口 (accept / skip / viewFront 等) 均为空实现或恒默认值.
 * 音频采样仍由真实音频解码器提供, 因此本类的 getSample 未实现.
 */
class VirtualVideoDecoder : public IDemuxDecoder {
private:
    /// 音频总时长 (秒) — 虚拟视频轨的时长
    qreal m_audioDuration;
public:
    /// 以音频时长构造虚拟视频轨
    VirtualVideoDecoder(qreal audioDuration) : m_audioDuration(audioDuration) {}

    /// 丢弃所有 Packet — 只要未被中断就持续"接收", 不做任何解码
    PONY_THREAD_SAFE bool accept(AVPacket *pkt, std::atomic<bool> &interrupt) override {
        return !interrupt;
    }

    /// 无需清空 FFmpeg 缓冲 (没有真实解码器)
    PONY_THREAD_SAFE void flushFFmpegBuffers() override {}

    /// 返回一个"有效但无图像"的帧, 让视频管线能正常推进 (无实际画面)
    PONY_THREAD_SAFE VideoFrameRef getPicture() override {
        return {nullptr, true, std::numeric_limits<qreal>::quiet_NaN()};
    }

    /// 音频采样由真实音频解码器提供, 此处不实现
    PONY_THREAD_SAFE AudioFrame getSample() override {
        NOT_IMPLEMENT_YET
    }

    /// 虚拟视频轨的时长等于音频总时长
    PONY_THREAD_SAFE double duration() override {
        return m_audioDuration;
    }

    /// 没有真实视频帧, 队首 PTS 恒为 NaN
    PONY_THREAD_SAFE qreal viewFront() override {
        return std::numeric_limits<qreal>::quiet_NaN();
    }

    /// 没有可跳过的帧, 恒返回 0
    PONY_THREAD_SAFE int skip(const std::function<bool(qreal)> &predicate) override {
        return 0;
    }

    /// 无实际解码输出, 启用开关为空操作
    PONY_THREAD_SAFE void setEnable(bool b) override {}

    /// 不涉及音频格式 (未实现)
    PONY_THREAD_SAFE PonyAudioFormat getInputFormat() override { NOT_IMPLEMENT_YET }

    /// 不涉及音频格式 (未实现)
    PONY_THREAD_SAFE void setOutputFormat(const PonyAudioFormat &format) override { NOT_IMPLEMENT_YET }

};
