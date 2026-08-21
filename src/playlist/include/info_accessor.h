//
// Created by 彭郑威 on 2022/4/30.
//

#ifndef PONYPLAYER_INFO_ACCESSOR_H
#define PONYPLAYER_INFO_ACCESSOR_H

#include "ponyplayer.h"

// 统一包裹 FFmpeg 头文件，避免宏污染 / 编译器差异
INCLUDE_FFMPEG_BEGIN
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/pixfmt.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
INCLUDE_FFMPEG_END
#include "playlist.h"

/**
 * @brief infoAccessor 媒体文件信息解析工具
 *
 * 封装对 FFmpeg 的调用，用于读取一个媒体文件（视频/音频）的元信息：
 * 帧率、码率、时长、封装格式、视频/音频流参数等，并把结果填充到 PlayListItem 中。
 */
class infoAccessor {
public:
    infoAccessor();

    /**
     * @brief 解析指定媒体文件的元信息
     * @param filename 媒体文件的路径（可能是 file:// URL，函数内部会转成本地路径）
     * @param res      出参：解析结果写入该 PlayListItem
     * @return 生成的预览帧图片路径（文件 URL 字符串），失败时为空字符串
     */
    static QString getInfo(QString filename, PlayListItem& res);
};

#endif //PONYPLAYER_INFO_ACCESSOR_H