#pragma once

#include <QtCore>
#include <utility>
#include "kv_engine.h"
#include <QThread>
#include <QMetaType>

/**
 * @brief PlayListItem 一条媒体文件的完整信息条目
 *
 * 继承自 ListItem（含 _uuid_ 主键），通过大量 Q_PROPERTY 暴露一个视频/音频文件的
 * 元信息字段（文件名、路径、时长、帧率、码率、视频/音频编码、尺寸、采样率等），
 * 这些属性会被 PonyKVConnect 序列化到数据库表中，也可被 QML 直接读取。
 */
class PlayListItem : public ListItem {
    Q_OBJECT
    Q_PROPERTY(QString _uuid_ READ getUUID WRITE setUUID)  // 唯一主键
    Q_PROPERTY(QString fileName READ getFileName WRITE setFileName)    // 文件名
    Q_PROPERTY(QString dir READ getDirectory WRITE setDirectoryByStr)  // 所在目录
    Q_PROPERTY(QString duration READ getDuration WRITE setDuration)  // 视频时长
    Q_PROPERTY(int frameRate READ getFrameRate WRITE setFrameRate)  // 视频帧率
    Q_PROPERTY(int bitRate READ getBitRate WRITE setBitRate)  // 视频码率
    Q_PROPERTY(float videoSize READ getVideoSize WRITE setVideoSize)  // 视频流大小
    Q_PROPERTY(int videoWidth READ getVideoWidth WRITE setVideoWidth)  // 视频宽度
    Q_PROPERTY(int videoHeight READ getVideoHeight WRITE setVideoHeight)  // 视频高度
    Q_PROPERTY(QString videoFormat READ getVideoFormat WRITE setVideoFormat)  // 视频编码格式
    Q_PROPERTY(QString audioFormat READ getAudioFormat WRITE setAudioFormat)  // 音频编码格式
    Q_PROPERTY(int audioAverageBitRate READ getAudioAverageBitRate WRITE setAudioAverageBitRate)  // 音频平均码率
    Q_PROPERTY(int channelNumbers READ getChannelNumbers WRITE setChannelNumbers)  // 音频通道数
    Q_PROPERTY(int sampleRate READ getSampleRate WRITE setSampleRate)  // 音频采样率
    Q_PROPERTY(float audioSize READ getAudioSize WRITE setAudioSize)  // 音频大小
    Q_PROPERTY(QString format READ getFormat WRITE setFormat)  // 封装格式
    Q_PROPERTY(QString path READ getPath WRITE setPath)  // 路径
    Q_PROPERTY(QString iconPath READ getIconPath WRITE setIconPath) // icon 路径


protected:
    // 以下成员一一对应上面的 Q_PROPERTY，保存实际的属性值
    QString fileName;           // 文件名
    QDir dir;                   // 所在目录
    QString path;               // 完整文件路径
    QString duration;           // 时长（格式 hh:mm:ss）
    int frameRate;              // 帧率
    int bitRate;                // 码率
    float videoSize;            // 视频流大小
    int videoWidth;             // 视频宽度
    int videoHeight;            // 视频高度
    QString videoFormat;        // 视频编码格式
    QString audioFormat;        // 音频编码格式
    int audioAverageBitRate;    // 音频平均码率
    int channelNumbers;         // 音频通道数
    int sampleRate;             // 音频采样率
    float audioSize;            // 音频大小
    int streamNumbers;          // 流数量
    QString format;             // 封装格式
    QString iconPath;           // 预览图路径

public:
    // 带文件名和目录的构造函数
    Q_INVOKABLE PlayListItem(QString _fileName, const QDir &_dir)
            : ListItem(), fileName(std::move(_fileName)), dir(_dir) {
    };

    // 默认构造函数
    Q_INVOKABLE PlayListItem() = default;

    Q_INVOKABLE QString getFileName();  // 获取文件名
    Q_INVOKABLE void setFileName(QString _fileName) { fileName = _fileName; }

    Q_INVOKABLE QString getIconPath() { return iconPath; }  // 获取预览图路径
    Q_INVOKABLE void setIconPath(QString _iconPath) { iconPath = _iconPath; }

    Q_INVOKABLE QString getPath() { return path; }  // 获取文件路径
    Q_INVOKABLE void setPath(QString _path) { path = _path; }

    Q_INVOKABLE QString getDirectory();  // 获取目录字符串
    Q_INVOKABLE void setDirectory(QDir _dir) { dir = _dir; }  // 设置目录
    Q_INVOKABLE void setDirectoryByStr(QString _dir) { dir = _dir; }  // 用字符串设置目录

    Q_INVOKABLE QString getDuration() { return duration; }  // 获取时长
    Q_INVOKABLE void setDuration(QString _duration) { duration = _duration; }  // 设置时长

    Q_INVOKABLE int getFrameRate() { return frameRate; }  // 获取帧率
    Q_INVOKABLE void setFrameRate(int _frameRate) { frameRate = _frameRate; }  // 设置帧率

    Q_INVOKABLE int getBitRate() { return bitRate; }  // 获取码率
    Q_INVOKABLE void setBitRate(int _bitRate) { bitRate = _bitRate; }  // 设置码率

    Q_INVOKABLE float getVideoSize() { return videoSize; }  // 获取视频流大小
    Q_INVOKABLE void setVideoSize(float _videoSize) { videoSize = _videoSize; }  // 设置视频流大小

    Q_INVOKABLE int getVideoWidth() { return videoWidth; }  // 获取视频宽度
    Q_INVOKABLE void setVideoWidth(int _videoWidth) { videoWidth = _videoWidth; }  // 设置视频宽度

    Q_INVOKABLE int getVideoHeight() { return videoHeight; }  // 获取视频高度
    Q_INVOKABLE void setVideoHeight(int _videoHeight) { videoHeight = _videoHeight; }  // 设置视频高度

    Q_INVOKABLE QString getVideoFormat() { return videoFormat; }  // 获取视频编码格式
    Q_INVOKABLE void setVideoFormat(QString _videoFormat) { videoFormat = _videoFormat; }  // 设置视频编码格式

    Q_INVOKABLE QString getAudioFormat() { return audioFormat; }  // 获取音频编码格式
    Q_INVOKABLE void setAudioFormat(QString _audioFormat) { audioFormat = _audioFormat; }  // 设置音频编码格式

    Q_INVOKABLE int getAudioAverageBitRate() { return audioAverageBitRate; }  // 获取音频平均码率
    Q_INVOKABLE void setAudioAverageBitRate(int _audioAverageBitRate) { audioAverageBitRate = _audioAverageBitRate; }  // 设置音频平均码率

    Q_INVOKABLE int getChannelNumbers() { return channelNumbers; }  // 获取音频通道数
    Q_INVOKABLE void setChannelNumbers(int _channelNumbers) { channelNumbers = _channelNumbers; }  // 设置音频通道数

    Q_INVOKABLE int getSampleRate() { return sampleRate; }  // 获取音频采样率
    Q_INVOKABLE void setSampleRate(int _sampleRate) { sampleRate = _sampleRate; }  // 设置音频采样率

    Q_INVOKABLE QString getFormat() { return format; }  // 获取封装格式
    Q_INVOKABLE void setFormat(QString _format) { format = _format; }  // 设置封装格式

    Q_INVOKABLE float getAudioSize() { return audioSize; }  // 获取音频大小
    Q_INVOKABLE void setAudioSize(float _audioSize) { audioSize = _audioSize; }  // 设置音频大小

    Q_INVOKABLE int getStreamNumbers() { return streamNumbers; }  // 获取流数量
    Q_INVOKABLE void setStreamNumbers(int _streamNumbers) { streamNumbers = _streamNumbers; }  // 设置流数量

    Q_INVOKABLE ~PlayListItem() override = default;

//    Q_INVOKABLE PlayListItem(const PlayListItem &listItem);
};

/*
 * 用于播放列表左侧条目简单信息
 * 只包含播放列表条目在左侧列表中展示所需的最小字段：文件名、路径、预览图。
 */
class simpleListItem: public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fileName READ getFileName WRITE setFileName)    // 文件名
    Q_PROPERTY(QString filePath READ getFilePath WRITE setFilePath)    // 文件路径
    Q_PROPERTY(QString iconPath READ getIconPath WRITE setIconPath)    // 预览图路径

public:
    // 默认构造函数
    Q_INVOKABLE simpleListItem() = default;

    // 带全部字段的构造函数
    Q_INVOKABLE simpleListItem(QString _fileName, QString _filePath, QString _iconPath)
            : fileName(_fileName), filePath(_filePath), iconPath(_iconPath) { };

    Q_INVOKABLE QString getFileName() { return fileName; }  // 获取文件名
    Q_INVOKABLE void setFileName(QString _fileName) { fileName = _fileName; }  // 设置文件名

    Q_INVOKABLE QString getFilePath() { return filePath; }  // 获取文件路径
    Q_INVOKABLE void setFilePath(QString _filePath) { filePath = _filePath; }  // 设置文件路径

    Q_INVOKABLE QString getIconPath() { return iconPath; }  // 获取预览图路径
    Q_INVOKABLE void setIconPath(QString _iconPath) { iconPath = _iconPath; }  // 设置预览图路径

    Q_INVOKABLE ~simpleListItem() = default;

    // 拷贝构造函数：复制各字段，保留父对象关系
    Q_INVOKABLE simpleListItem(const simpleListItem& other): QObject(other.parent()) {
        fileName = other.fileName;
        filePath = other.filePath;
        iconPath = other.iconPath;
    }



private:
    QString fileName;   // 文件名
    QString filePath;   // 文件路径
    QString iconPath;   // 预览图路径
};
Q_DECLARE_METATYPE(simpleListItem);

/**
 * @brief PlayList 播放列表后端数据操作者
 *
 * 运行在后台线程中，封装对 PonyKVList<PlayListItem> 的增删查操作，
 * 供 Controller 通过信号-槽跨线程调用，并通过一系列 *_Done 信号把结果发回前端。
 */
class PlayList : public QObject {
    Q_OBJECT
//    Q_PROPERTY(QQmlListProperty<QObject*> playListItems READ playListItems)

private:
    QList<QObject *> data;                       // 条目数据缓存
    PonyKVList<PlayListItem> pkvList;            // 绑定到 PlayListItem 表的存取对象

//    static void appendPlayList(QQmlListProperty<QObject*>* list,QObject *qobj);
//    static int playListCount(QQmlListProperty<QObject*>*) const;
//    static QObject* listItem(QQmlListProperty<QObject*>*,int) const;
//    static void clearPlayList(QQmlListProperty<QObject*>*);
public:
//    QList<ListItem> getData();
    PlayList(QString _dbName, QString _tableName, QString _className);
//    QQmlListProperty<QObject*> playListItems();

//    void appendPlayList(QObject *qobj);
//    int playListCount() const;
//    QObject* listItem(int) const;
//    void clearPlayList();

//    void insert(ListItem item);

    explicit PlayList(const QString &name);

public slots:
    void insert(PlayListItem *item);        // 插入一个媒体条目
    void remove(QString filepath);          // 按路径删除媒体条目
    PlayListItem* search(QString key);      // 按主键搜索（当前未实现，恒返回 nullptr）
    void extractAndProcess();               // 提取所有条目并转换为 simpleListItem
    void getInfo(QString path);             // 按路径获取媒体详细信息

signals:
    void insertDone(int resultcode);                 // 插入完成，携带结果码
    void removeDone(int resultcode);                 // 删除完成，携带结果码
    void searchDone(PlayListItem *item);             // 搜索完成，携带结果
    void extractDone(QList<simpleListItem*> res);    // 提取完成，携带简单条目列表
    void getInfoDone(PlayListItem *item);            // 获取信息完成，携带详细条目
};

Q_DECLARE_METATYPE(PlayListItem*)