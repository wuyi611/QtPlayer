//
// Created by 彭郑威 on 2022/4/29.
//

#ifndef PONYPLAYER_CONTROLLER_H
#define PONYPLAYER_CONTROLLER_H

#include <QObject>
#include <QThread>
#include <QDebug>
#include "playlist.h"
#include "info_accessor.h"

/**
 * @brief Controller 媒体库前端控制器
 *
 * 作为 QML 前端与 PlayList 后端之间的桥接层：
 *  - 负责把 QML 发起的请求（插入/删除/搜索/提取/获取信息）转发到后台线程的 PlayList；
 *  - 接收 PlayList 处理完成后的结果并缓存，供 QML 通过 Q_INVOKABLE 方法拉取；
 *  - 本身运行在 UI 线程，实际数据库操作全部在 listOPThread 线程中完成。
 */
class Controller : public QObject {
Q_OBJECT
    // 暴露给 QML 的最近打开文件列表属性（只读，变化时发出 recentFilesChanged）
    Q_PROPERTY(QVariantList recentFiles READ getRecentFiles NOTIFY recentFilesChanged)
    // 后台工作线程：PlayList 在该线程中执行数据库操作，避免阻塞 UI
    QThread listOPThread;

private:
    // 提取操作完成后缓存的简单条目列表（供 QML 拉取）
    QList<simpleListItem *> result;
    // 最近一次获取的媒体详细信息条目
    PlayListItem *playListItemResult;

public:
    explicit Controller(QObject *parent = nullptr);

    ~Controller();

    // 返回媒体库中所有条目的简化列表（QML ListView 数据源）
    Q_INVOKABLE QVariantList getSimpleListItemList();

    // 获取最近打开的文件列表
    Q_INVOKABLE QVariantList getRecentFiles();

    // 向最近打开的文件记录中写入最新的一条
    Q_INVOKABLE void updateRecentFile(QString filePath);

    // 获取当前选中媒体条目的详细信息
    Q_INVOKABLE QVariantMap getListItemInfo();

public slots:

    // 插入操作完成后的回调：根据结果码打印成功/失败日志
    void getInsertRst(int resultCode) {
        if (resultCode == 0)
            qDebug() << "Insert to MediaLib Success!\n";
        else
            qDebug() << "Insert to MediaLib Fail!\n";
    }

    // 删除操作完成后的回调：根据结果码打印成功/失败日志
    void getRemoveRst(int resultCode) {
        if (resultCode == 0)
            qDebug() << "Remove from MediaLib Success!\n";
        else
            qDebug() << "Remove from MediaLib Fail!\n";
    }

    // 搜索操作完成后的回调：根据返回条目是否为 nullptr 打印结果
    void getSearchRst(PlayListItem *resultItem) {
        if (!resultItem)
            qDebug() << "Search Find!\n";
        else
            qDebug() << "Search Fail!\n";
    }

    // 提取操作完成后的回调：缓存所有简单条目并通知 QML 刷新
    void getExtractRst(QList<simpleListItem *> rst) {
        result.clear();

        for (int i = 0; i < rst.size(); i++) {
            result.append(rst[i]);
        }
        emit finishExtractItems();
    }

    // 获取信息完成后的回调：缓存详细条目并通知 QML 刷新
    void getInfoRst(PlayListItem *rst) {
        playListItemResult = rst;
        emit finishGetInfo();
    }

    // 解析指定媒体文件：调用 infoAccessor 提取元信息，打印调试日志并插入媒体库
    QString getFile(QString filename, QString path) {
        PlayListItem *info = new PlayListItem;
        info->setFileName(filename);
        info->setPath(path);
        QString iconPath = infoAccessor::getInfo(path, *info);
        qDebug() << "文件:" << info->getFileName();
        qDebug() << "路径:" << info->getPath();
        qDebug() << "帧率:" << info->getFrameRate();
        qDebug() << "比特率:" << info->getBitRate();
        qDebug() << "视频流大小:" << info->getVideoSize();
        qDebug() << "画面尺寸:" << info->getVideoWidth() << "*" << info->getVideoHeight();
        qDebug() << "视频格式:" << info->getVideoFormat();
        qDebug() << "音频格式:" << info->getAudioFormat();
        qDebug() << "音频平均比特率:" << info->getAudioAverageBitRate();
        qDebug() << "音频通道数:" << info->getChannelNumbers();
        qDebug() << "音频采样率:" << info->getSampleRate();
        qDebug() << "音频流大小:" << info->getAudioSize();
        qDebug() << "流数量:" << info->getStreamNumbers();
        emit insertItem(info);
        return iconPath;
    }

    // 发送“提取媒体库所有条目”的请求信号
    void sendExtractRequirement() { emit extractRequirement(); }

    // 发送“删除指定路径条目”的请求信号
    void sendRemoveRequirement(QString filepath, QString iconPath) { emit removeRequirement(filepath); }

    // 发送“获取指定路径媒体详细信息”的请求信号
    void sendGetInfoRequirement(QString filepath) { emit getInfoRequirement(filepath); }

signals:

    // 发送信号触发后台线程插入媒体条目
    void insertItem(PlayListItem *item);

    // 发送信号触发后台线程删除媒体条目
    void removeItem(PlayListItem *item);

    // 发送信号触发后台线程按主键搜索
    void searchItem(QString primaryKey);

    void extractRequirement();   // 向 PlayList 发送的提取请求

    void finishExtractItems();   // 向 qml 发送的提取完毕的信号

    void removeRequirement(QString path);   // 向 PlayList 发送的删除请求

    void getInfoRequirement(QString path);  // 向 PLayList 发送获取信息请求

    void finishGetInfo();  // 向 qml 发送查找完毕的信号

    void recentFilesChanged();  // 最近打开文件列表发生变化
};

#endif //PONYPLAYER_CONTROLLER_H