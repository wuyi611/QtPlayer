/**
 * @file frame.hpp
 * @brief 音视频帧数据结构 — 解码层产出的帧在队列 / 渲染管线之间传递的载体
 *
 * 包含三个类型:
 *   - VideoFrame      : 引用计数的 AVFrame 包装器 (独占一个 AVFrame 的所有权)
 *   - VideoFrameRef   : RAII 视频帧引用句柄, 支持拷贝 / 移动 (共享外层引用计数),
 *                       是视频帧跨线程传递时的标准类型
 *   - AudioFrame      : 裸 PCM 采样数据的轻量封装 (值语义, 不持有内存)
 *
 * 生命周期约定:
 *   - 解码器解出的 AVFrame 入队后, getPicture 将其包成 VideoFrameRef 移交上层;
 *     外层引用计数归零时析构内部调用 av_frame_free 真正释放 AVFrame.
 *   - AudioFrame 指向解码器内部复用的 audioOutBuf (一次性缓冲),
 *     下一次 getSample 会覆盖其内容, 消费端必须尽快使用.
 */
//
// Created by ColorsWind on 2022/5/7.
//
#pragma once

#include "helper.hpp"
#include "ponyplayer.h"
#include <mutex>
#include <functional>
#include <queue>

INCLUDE_FFMPEG_BEGIN
#include <libavformat/avformat.h>
INCLUDE_FFMPEG_END

/// 帧释放回调类型 — 队列 clear / skip 时归还 AVFrame 给 FFmpeg (通常为 av_frame_free)
typedef std::function<void(AVFrame *)> FrameFreeFunc;

/**
 * @brief 引用计数的 AVFrame 包装器 — 一个 VideoFrame 独占一个 AVFrame 的所有权
 *
 * 生命周期:
 *   - 构造时 m_refCount = 1, 之后由 VideoFrameRef 管理;
 *   - VideoFrameRef 每次拷贝 m_refCount +1, 移动则转移所有权不增计数;
 *   - unref() 减计数, 归零时 delete this → 析构 av_frame_free 释放 AVFrame.
 *
 * 注意: 本类的外层引用计数与 FFmpeg 内部 AVFrame 的 buffer 引用计数相互独立,
 *       外层计数只决定"何时对 AVFrame 调用 av_frame_free".
 */
class VideoFrame {
    AVFrame *m_frame;             ///< 持有的 AVFrame (本类独占所有权)
    std::atomic<int> m_refCount;  ///< 外层引用计数 (由 VideoFrameRef 增减, 原子类型线程安全)
    const double m_pts;           ///< 帧显示时间戳 (秒)
    const bool m_isValid;         ///< 帧是否有效 (用于区分"无图像数据"的空帧)

    friend class VideoFrameRef;
public:
    /// 全局存活实例计数 (调试用: 检测帧泄漏)
    static inline std::atomic<int> totalCount = 0;

    /// 构造 — 接管传入 AVFrame 的所有权, 初始引用计数为 1
    VideoFrame(AVFrame *frame, double pts, const bool isValid)
            : m_frame(frame), m_refCount(1), m_pts(pts), m_isValid(isValid) {
        if (frame) ++totalCount;
    }

    /// 析构 — 释放持有的 AVFrame (递减全局计数并调用 av_frame_free)
    ~VideoFrame() {
        if (m_frame) {
            --totalCount;
            av_frame_free(&m_frame);
        }
    }

    /// 释放一个引用; 计数归零时销毁自身 (delete this)
    void unref() {
        if (--m_refCount == 0) {
            delete this;
        }
    }
};

/**
 * @brief RAII 视频帧引用句柄 — 视频帧在解码 / 渲染管线之间传递的标准类型
 *
 * 语义:
 *   - 拷贝构造 / 拷贝赋值: 共享底层 VideoFrame, 外层引用计数 +1;
 *   - 移动构造 / 移动赋值: 转移所有权, 源句柄置空, 不增计数;
 *   - 析构: 调用 VideoFrame::unref, 引用归零时底层帧被释放.
 * 用法: 值传递 (栈上传递, 跨线程时经信号槽按值转发), 调用方无需手动管理内存.
 */
class VideoFrameRef {
private:
    VideoFrame *m_videoFrame;   ///< 底层引用计数帧 (nullptr 表示空句柄)
public:
    /// 构造 — 用解码出的 AVFrame 新建一个引用计数帧 (接管所有权, 初始计数 1)
    VideoFrameRef(AVFrame *frame, bool isValid, double pts) {
        m_videoFrame = new VideoFrame(frame, pts, isValid);
    }

    /// 默认构造 — 空句柄 (无效帧, isValid() == false)
    VideoFrameRef() : VideoFrameRef(nullptr, false, std::numeric_limits<double>::quiet_NaN()) {}

    /// 移动构造 — 转移所有权, 源句柄置空 (不增加引用计数)
    VideoFrameRef(VideoFrameRef &&rhs) noexcept: m_videoFrame(rhs.m_videoFrame) {
        rhs.m_videoFrame = nullptr;
    }

    /// 拷贝构造 — 共享底层帧, 引用计数 +1
    VideoFrameRef(const VideoFrameRef &rhs) : m_videoFrame(rhs.m_videoFrame) {
        ++rhs.m_videoFrame->m_refCount;
    }

    /// 移动赋值 — 先释放自身旧引用, 再接管 rhs 所有权
    VideoFrameRef &operator=(VideoFrameRef &&rhs) noexcept {
        if (this->m_videoFrame) { this->m_videoFrame->unref(); }
        this->m_videoFrame = rhs.m_videoFrame;
        rhs.m_videoFrame = nullptr;
        return *this;
    }

    /// 拷贝赋值 — 释放自身旧引用后共享 rhs 的帧, 引用计数 +1 (自赋值安全)
    VideoFrameRef &operator=(const VideoFrameRef &rhs) noexcept {
        if (&rhs != this) {
            if (this->m_videoFrame) { this->m_videoFrame->unref(); }
            this->m_videoFrame = rhs.m_videoFrame;
            ++m_videoFrame->m_refCount;
        }
        return *this;
    }


    /// 析构 — 释放一个引用, 归零时底层 VideoFrame 被销毁
    ~VideoFrameRef() {
        if (m_videoFrame) { m_videoFrame->unref(); }
    }

    /// 两个句柄是否指向同一个底层帧
    bool operator==(const VideoFrameRef &frame) const {
        return this->m_videoFrame == frame.m_videoFrame;
    }

    bool operator!=(const VideoFrameRef &frame) const {
        return !this->operator==(frame);
    }

    /**
     * @return 图像数据是否有效
     */
    [[nodiscard]] bool isValid() const {
        return m_videoFrame && m_videoFrame->m_isValid;
    }


    /// @return 帧显示时间戳 (秒)
    [[nodiscard]] double getPTS() const {
        return m_videoFrame->m_pts;
    }

    /// @return Y 平面数据指针 (YUV420P 等格式下有效; 空帧返回 nullptr)
    [[nodiscard]] std::byte *getY() const {
        return !m_videoFrame->m_frame ? nullptr : reinterpret_cast<std::byte *>(m_videoFrame->m_frame->data[0]);
    }

    /// @return U 平面数据指针 (色度平面; 空帧返回 nullptr)
    [[nodiscard]] std::byte *getU() const {
        return !m_videoFrame->m_frame ? nullptr : reinterpret_cast<std::byte *>(m_videoFrame->m_frame->data[1]);
    }

    /// @return V 平面数据指针 (色度平面; 空帧返回 nullptr)
    [[nodiscard]] std::byte *getV() const {
        return !m_videoFrame->m_frame ? nullptr : reinterpret_cast<std::byte *>(m_videoFrame->m_frame->data[2]);
    }

    /// @return Y 平面的行跨度 (字节/行), 上传纹理时用于定位每行起点
    [[nodiscard]] int getLineSize() const {
        return !m_videoFrame->m_frame ? 0 : m_videoFrame->m_frame->linesize[0];
    }

    /// @return 帧宽度 (像素)
    [[nodiscard]] int getWidth() const {
        return !m_videoFrame->m_frame ? 0 : m_videoFrame->m_frame->width;
    }

    /// @return 帧高度 (像素)
    [[nodiscard]] int getHeight() const {
        return !m_videoFrame->m_frame ? 0 : m_videoFrame->m_frame->height;
    }

    /**
     * @brief 判断两帧尺寸是否一致 (宽 / 高 / 行跨度全同)
     * @return 两帧都有效且尺寸相同, 或两帧都无效
     * @note 渲染器据此决定纹理是否需要重新分配, 还是仅更新子区域
     */
    [[nodiscard]] bool isSameSize(const VideoFrameRef &frame) const {
        return (this->isValid()
                && frame.isValid()
                && this->getWidth() == frame.getWidth()
                && this->getHeight() == frame.getHeight()
                && this->getLineSize() == frame.getLineSize())
               || (!this->isValid() && !frame.isValid());
    }

    /// @brief 判断本帧尺寸是否等于给定的宽高 (供渲染器预分配纹理时使用)
    [[nodiscard]] bool isSameSize(int width, int height) const {
        return this->isValid() && this->getWidth() == width && this->getHeight() == height;
    }

};

/**
 * @brief 音频帧 — 裸 PCM 采样数据的轻量封装 (值语义)
 *
 * 注意: 本类**不持有内存** — 数据指针指向解码器内部复用的 audioOutBuf,
 *       下一次 getSample 调用会覆盖该缓冲, 消费端必须立即使用,
 *       且不能跨线程长期持有 AudioFrame 指向的数据.
 */
class AudioFrame {
private:
    std::byte *m_data;   ///< PCM 采样数据指针 (指向一次性缓冲, 不拥有所有权)
    int m_len;           ///< 数据字节长度
    double m_pts;        ///< 该块采样的起始时间戳 (秒)
public:
    /// 默认构造 — 空音频帧 (isValid() == false)
    AudioFrame() : m_data(nullptr), m_len(0), m_pts(std::numeric_limits<double>::quiet_NaN()) {}

    /// 构造 — 指向一段 PCM 采样 (浅拷贝语义, 不复制数据)
    AudioFrame(std::byte *data, int len, double pts) : m_data(data), m_len(len), m_pts(pts) {}

    /// @return 是否包含有效采样数据
    bool isValid() {
        return m_data;
    }

    /// @return PCM 采样数据指针
    [[nodiscard]] std::byte *getSampleData() const {
        return m_data;
    }

    /// @return 采样数据字节长度
    [[nodiscard]] int getDataLen() const {
        return m_len;
    }

    /// @return 采样起始时间戳 (秒)
    [[nodiscard]] double getPTS() const {
        return m_pts;
    }
};

