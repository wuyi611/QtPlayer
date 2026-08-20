//
// Created by ColorsWind on 2022/5/15.
//
// audioformat.hpp - 音频格式定义
//
// 定义跨 FFmpeg 与 PortAudio 的音频格式封装:
// - PonySampleFormat: 采样格式(位深/类型)的统一描述, 同时持有 FFmpeg 与 PortAudio 两边的枚举值
// - PonyAudioFormat:  采样格式 + 采样率 + 声道数的完整描述, 提供字节数⇄时长换算
// - AudioDataInfo:    记录 Sonic 变速前后数据长度映射, 供回调折算播放位置
// - PonyPlayer 命名空间: 预定义常用格式常量与枚举转换函数
//
#pragma once

#include <QtCore>
#include <utility>
#include "portaudio.h"

INCLUDE_FFMPEG_BEGIN
#include "libavutil/samplefmt.h"
INCLUDE_FFMPEG_END



/// 一段音频数据的变速前后长度信息(由 write 时入队, PA 回调消费)
struct AudioDataInfo {
    qint32 origLength;      ///< 变速前(原始)字节数
    qint32 processedLength; ///< 变速后(实际写入环形缓冲)字节数
    qreal speedUpRate;      ///< 变速比率 = origLength / processedLength
};

/**
 * @brief 采样格式的统一描述(位深/类型)。
 *
 * 同时保存三种表示:
 * - m_paSampleFormat: PortAudio 的 PaSampleFormat 枚举
 * - m_ffmpegSampleFormat: FFmpeg 的 AVSampleFormat 枚举
 * - m_bytesPerSample: 单样本字节数
 * 并携带一个按因子缩放采样值的变换函数(用于音量调整)。
 * 通过静态工厂 of<T>() 以模板参数 T 决定字节数与缩放函数。
 */
struct PonySampleFormat {
private:
    using TransformFunc = std::function<void(std::byte *, qreal, unsigned long)>;

    int m_index;                        ///< 唯一索引(用于 == 比较)
    PaSampleFormat m_paSampleFormat;    ///< PortAudio 格式枚举
    AVSampleFormat m_ffmpegSampleFormat;///< FFmpeg 格式枚举
    int m_bytesPerSample;               ///< 单样本字节数
    std::function<void(std::byte *, qreal, unsigned long)> m_transform; ///< 采样值缩放函数(音量)


    /// 私有构造, 只能通过 of<T>() 创建
    PonySampleFormat(
            int mIndex,
            PaSampleFormat paSampleFormat,
            AVSampleFormat ffmpegSampleFormat,
            int bytesPerSample,
            TransformFunc transformFunc
    ) : m_index(mIndex),
        m_paSampleFormat(paSampleFormat),
        m_ffmpegSampleFormat(ffmpegSampleFormat),
        m_bytesPerSample(bytesPerSample),
        m_transform(std::move(transformFunc)) {}

public:
    /**
     * 以采样类型 T 创建格式描述。
     * T = void 表示"未知/不支持"格式(变换函数直接抛异常);
     * 否则 T 决定单样本字节数与缩放实现(样本值 × factor)。
     */
    template<class T>
    static PonySampleFormat of(PaSampleFormat paSample, AVSampleFormat ffmpegSample) noexcept {
        static int id = 0;
        TransformFunc transform;
        size_t size;
        if constexpr(std::is_same<T, void>()) {
            transform = [](std::byte *src_, qreal factor, unsigned long samples) {
                throw std::runtime_error("Unsupported samples format.");
            };
            size = 0xABCDEF;
        } else {
            transform = [](std::byte *src_, qreal factor, unsigned long samples) {
                T *src = static_cast<T *>(static_cast<void *>(src_));
                for (size_t sampleOffset = 0; sampleOffset < samples; sampleOffset++) {
                    src[sampleOffset] = static_cast<T>(src[sampleOffset] * factor);
                }
            };
            size = sizeof(T);
        }
        return {id, paSample, ffmpegSample, static_cast<int>(size), transform};
    }

    /// 对 samples 个样本应用缩放因子(音量调整)
    void transformSampleVolume(std::byte *src, qreal factor, unsigned long samples) const {
        m_transform(src, factor, samples);
    }

    /// 按索引比较是否同一格式
    bool operator==(const PonySampleFormat &rhs) const {
        return this->m_index == rhs.m_index;
    }

    bool operator!=(const PonySampleFormat &rhs) const {
        return !(rhs == *this);
    }

    /// 获取 PortAudio 格式枚举
    [[nodiscard]] PaSampleFormat getPaSampleFormat() const {
        return m_paSampleFormat;
    }

    /// 获取 FFmpeg 格式枚举
    [[nodiscard]] AVSampleFormat getFFmpegSampleFormat() const {
        return m_ffmpegSampleFormat;
    }

    /// 获取单样本字节数
    [[nodiscard]] int getBytesPerSample() const {
        return m_bytesPerSample;
    }

};



/**
 * @brief 完整音频格式: 采样格式 + 采样率 + 声道数。
 *
 * 提供:
 * - 面向 PortAudio / FFmpeg 的格式枚举获取
 * - 字节数 ⇄ 时长换算(durationOfBytes / bytesOfDuration)
 * - 环形缓冲建议容量(suggestedRingBuffer, 按倍速预留)
 */
class PonyAudioFormat {
private:
    PonySampleFormat m_sampleFormat; ///< 采样格式
    int m_sampleRate;                ///< 采样率(Hz)
    int m_channelCount;              ///< 声道数

public:

    /// 构造: 采样格式 + 采样率 + 声道数
    PonyAudioFormat(
            PonySampleFormat sampleFormat,
            int sampleRate,
            int channelCount
    ) noexcept: m_sampleFormat(std::move(sampleFormat)), m_sampleRate(sampleRate), m_channelCount(channelCount) {}

    /// 获取采样格式
    [[nodiscard]] const PonySampleFormat &getSampleFormat() const { return m_sampleFormat; }

    /// 获取 PortAudio 采样格式枚举
    [[nodiscard]] PaSampleFormat getSampleFormatForPA() const {
        return m_sampleFormat.getPaSampleFormat();
    }

    /// 获取 FFmpeg 采样格式枚举
    [[nodiscard]] AVSampleFormat getSampleFormatForFFmpeg() const {
        return m_sampleFormat.getFFmpegSampleFormat();
    }

    /// 字节数 → 时长(秒): bytes / (采样率 × 声道数 × 单样本字节数)
    [[nodiscard]] qreal durationOfBytes(int64_t bytes) const {
        return static_cast<qreal>(bytes) / (m_sampleRate * m_channelCount * getBytesPerSample());
    }

    /// 时长(秒) → 字节数
    [[nodiscard]] int64_t bytesOfDuration(qreal duration) const {
        return static_cast<int64_t>(duration * m_sampleRate * m_channelCount * getBytesPerSample());
    }

    /// 获取单样本字节数
    [[nodiscard]] int getBytesPerSample() const {
        return m_sampleFormat.getBytesPerSample();
    }

    /// 获取单帧(一个时间点的所有声道)字节数 = 单样本字节数 × 声道数
    [[nodiscard]] int getBytesPerSampleChannels() const {
        return m_sampleFormat.getBytesPerSample() * m_channelCount;
    }

    /// 获取采样率
    [[nodiscard]] int getSampleRate() const {
        return m_sampleRate;
    }

    /// 获取声道数
    [[nodiscard]] int getChannelCount() const {
        return m_channelCount;
    }

    /**
     * 计算环形缓冲建议容量(字节, 下限 2K×声道×样本字节, 上限 256MB)。
     * @param speedFactor 倍速 — 倍速越高缓冲需求越大(0.2 × 倍速 秒的音频量)
     */
    [[nodiscard]] int64_t suggestedRingBuffer(qreal speedFactor) const {
        return qBound<int64_t>(
                static_cast<int64_t>(2 * 1024 * m_channelCount * m_sampleFormat.getBytesPerSample()),
                bytesOfDuration(0.2 * speedFactor),
                256 << 20
        );
    }
};

namespace PonyPlayer {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    // 预定义的采样格式常量(双端枚举 + 字节数 + 缩放函数)
    const PonySampleFormat Unknown = PonySampleFormat::of<void>(paNonInterleaved, AV_SAMPLE_FMT_NONE);
    const PonySampleFormat UInt8 = PonySampleFormat::of<uint8_t>(paUInt8, AV_SAMPLE_FMT_U8);
    const PonySampleFormat Int16 = PonySampleFormat::of<int16_t>(paInt16, AV_SAMPLE_FMT_S16);
    const PonySampleFormat Int32 = PonySampleFormat::of<int32_t>(paInt32, AV_SAMPLE_FMT_S32);
    const PonySampleFormat Float = PonySampleFormat::of<float_t>(paFloat32, AV_SAMPLE_FMT_FLT);
#pragma GCC diagnostic pop
    /// 默认音频格式: Int16 / 44100Hz / 双声道
    const PonyAudioFormat DEFAULT_AUDIO_FORMAT = {Int16, 44100, 2};

    /// FFmpeg 采样格式枚举 → PonySampleFormat(平面格式与交错格式合并映射)
    static PonySampleFormat valueOf(AVSampleFormat ffmpegFormat) {
        switch (ffmpegFormat) {
            case AV_SAMPLE_FMT_U8:
            case AV_SAMPLE_FMT_U8P:
                return UInt8;
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                return Int16;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                return Int32;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                return Float;
            default:
                return Unknown;
        }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    /// PortAudio 采样格式枚举 → PonySampleFormat
    static PonySampleFormat valueOf(PaSampleFormat paSampleFormat) {
        switch (paSampleFormat) {
            case paUInt8:
                return UInt8;
            case paInt16:
                return Int16;
            case paInt32:
                return Int32;
            case paFloat32:
                return Float;
            default:
                return Unknown;
        }

    }
#pragma GCC diagnostic pop
}
