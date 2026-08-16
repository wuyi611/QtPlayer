//
// Created by 彭郑威 on 2022/4/29.
//

#include "controller.h"

/**
 * @brief Controller 构造函数
 *
 * 初始化媒体库控制器，主要完成以下工作：
 * 1. 创建 PlayList 实例作为媒体库的后端数据操作者
 * 2. 将 PlayList 移至独立的工作线程（listOPThread），避免阻塞 UI 线程
 * 3. 建立 Controller（前端信号）与 PlayList（后端槽）之间的跨线程信号-槽连接
 * 4. 建立 PlayList 操作完成信号与 Controller 结果处理槽的连接
 * 5. 启动工作线程并发射提取请求信号，加载媒体库数据
 *
 * @param parent 父 QObject，用于 Qt 对象树的内存管理
 */
Controller::Controller(QObject *parent) : QObject(parent)
{
    // 创建媒体库数据操作对象，指定数据库名、表名和条目类型
    PlayList *listOPer = new PlayList("MediaLib","MediaInfo","PlayListItem");
    // 将 PlayList 对象移至独立线程，使其操作在后台执行
    listOPer->moveToThread(&listOPThread);

    // ========== Controller → PlayList：前端请求信号连接到后端处理槽 ==========
    // 插入媒体条目
    connect(this, SIGNAL(insertItem(PlayListItem*)), listOPer, SLOT(insert(PlayListItem*)));
//    connect(this, SIGNAL(removeItem(PlayListItem*)), listOPer, SLOT(remove(PlayListItem*)));
    // 搜索媒体条目
    connect(this, SIGNAL(searchItem(QString)), listOPer, SLOT(search(QString)));
    // 提取并处理所有媒体库条目
    connect(this, SIGNAL(extractRequirement()), listOPer, SLOT(extractAndProcess()));
    // 删除指定路径的媒体条目
    connect(this, SIGNAL(removeRequirement(QString)), listOPer, SLOT(remove(QString)));
    // 获取指定路径的媒体详细信息
    connect(this, SIGNAL(getInfoRequirement(QString)), listOPer, SLOT(getInfo(QString)));

    // ========== PlayList → Controller：后端操作完成信号连接到前端结果处理槽 ==========
    // 线程结束时销毁 PlayList 对象，防止内存泄漏
    connect(&listOPThread, &QThread::finished, listOPer, &QObject::deleteLater);

    // 插入操作完成
    connect(listOPer, SIGNAL(insertDone(int)), this, SLOT(getInsertRst(int)));
    // 删除操作完成
    connect(listOPer, SIGNAL(removeDone(int)), this, SLOT(getRemoveRst(int)));
    // 搜索操作完成，返回搜索结果
    connect(listOPer, SIGNAL(searchDone(PlayListItem*)), this, SLOT(getSearchRst(PlayListItem*)));
    // 提取操作完成，返回媒体库中所有条目的简化列表
    connect(listOPer, SIGNAL(extractDone(QList<simpleListItem*>)), this, SLOT(getExtractRst(QList<simpleListItem*>)));
    // 获取详细信息操作完成
    connect(listOPer, SIGNAL(getInfoDone(PlayListItem*)), this, SLOT(getInfoRst(PlayListItem*)));

    // 启动后台工作线程
    listOPThread.start();
    // 发射提取请求信号，开始加载媒体库中的所有条目
    qDebug()<<"-------------- MediaLib Thread Start! ID:"<<QThread::currentThreadId()<<"--------------\n";
}

/**
 * @brief Controller 析构函数
 *
 * 安全退出后台工作线程：调用 quit() 请求线程退出，然后 wait() 阻塞等待线程真正结束。
 * 这确保了在 Controller 销毁时，所有后台数据库操作已安全完成。
 */
Controller::~Controller()
{
    listOPThread.quit();
    listOPThread.wait();
    qDebug()<<"-------------- MediaLib Thread Finish! --------------\n";
}

/**
 * @brief 获取媒体库中所有条目的简化列表
 *
 * 将内部的 QList<simpleListItem*> 结果转换为 QVariantList，
 * 供 QML 端的 ListView 等组件直接使用。
 *
 * @return QVariantList 包含所有媒体库条目的简化信息列表
 */
QVariantList Controller::getSimpleListItemList() {
    QVariantList res;
    for(int i=0;i<result.size();i++){
        res.append(QVariant::fromValue(result[i]));
    }

    return res;
}

/**
 * @brief 获取最近打开的文件列表
 *
 * 从 PONYPATH 环境变量指定的目录下读取 recentOpenFiles.txt 文件，
 * 解析每一行的文件名和路径（以逗号分隔），返回供 QML 显示的列表。
 * 文件格式：每行 "文件名,文件路径"
 *
 * @return QVariantList 最近打开的文件列表，每个元素为 [文件名, 文件路径]
 */
QVariantList Controller::getRecentFiles() {
    QVariantList res;

//    QString source = QDir::currentPath() + "/recentOpenFiles.txt";
    // 从环境变量 PONYPATH 获取数据存储目录
    QString home = qEnvironmentVariable("PONYPATH");
    QString source = home  + "/recentOpenFiles.txt";
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug()<< "Can't open the file!";
    }

    // 逐行读取文件内容
    QTextStream in(&file);
    while (true) {
        QString line = in.readLine();
        if (line.isNull())
            break;
        // 每行按逗号分隔为文件名和路径
        QStringList parts = line.split(",");
        QVariantList temp;
        for(int i=0;i<parts.size();i++)
            temp.append(QVariant::fromValue<QString>(parts[i]));
        res.append(QVariant::fromValue<QVariantList>(temp));
    }
    file.close();
    return res;
}

/**
 * @brief 更新最近打开的文件记录
 *
 * 将新打开的文件写入 recentOpenFiles.txt 的最前面，同时保留最近最多 10 条记录。
 * 如果文件已存在于历史记录中，则将其移到最前面（去重）。
 * 写入完成后发射 recentFilesChanged 信号通知 QML 刷新显示。
 *
 * @param filePath 新打开文件的完整路径
 */
void Controller::updateRecentFile(QString filePath) {
    QUrl url(filePath);
    QString fileName = QFileInfo(url.path()).fileName();  // 从路径中提取文件名
    QString home = qEnvironmentVariable("PONYPATH");
//    QString source = QDir::currentPath() + "/recentOpenFiles.txt";
    QString source = home + "/recentOpenFiles.txt";
    QFile file(source);
    QStringList readyWrite;  // 将要写入文件的内容列表
    // 新打开的文件放在第一条
    readyWrite.append(fileName+","+filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug()<< "Can't open the file!";
    }

    // 读取现有历史记录，跳过重复项，最多保留 9 条旧记录（总共 10 条）
    QTextStream in(&file);
    int count=0;
    while (true) {
        QString line = in.readLine();
        if (line.isNull())
            break;
        // 跳过与新添加文件重复的记录（去重）
        if(line==readyWrite[0])
            continue;
        else {
            if(count<9) {
                readyWrite.append(line);
                count += 1;
            }
            else
                break;
        }
    }
    file.close();

    // 以覆盖模式重新打开文件，写入更新后的记录
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
    {
        qDebug()<< "Can't open the file!";
    }
    QTextStream out(&file);
    for(int i=0;i<readyWrite.size();i++) {
        out<<readyWrite[i]<<"\n";
    }
    file.close();
    // 通知 QML 端最近文件列表已更新
    emit recentFilesChanged();
}

/**
 * @brief 获取当前选中媒体条目的详细信息
 *
 * 从 playListItemResult 中提取视频和音频的详细参数，
 * 以 QVariantMap 形式返回，供 QML 端显示媒体信息面板。
 * 包含：文件名、路径、时长、封装格式、视频帧率/码率/尺寸/编码、
 * 音频编码/码率/通道数/采样率/流大小等。
 *
 * @return QVariantMap 键为中文标签，值为对应的媒体信息
 */
QVariantMap Controller::getListItemInfo() {
    QVariantMap res;
    qDebug() << playListItemResult->getVideoWidth() << "*" << playListItemResult->getVideoHeight();
    // 视频基本信息
    res["文件名"] = QVariant::fromValue(playListItemResult->getFileName());
    res["文件路径"] = QVariant::fromValue(playListItemResult->getPath());
    res["时长"] = QVariant::fromValue(playListItemResult->getDuration());
    res["封装格式"] = QVariant::fromValue(playListItemResult->getFormat());
    // 视频流参数
    res["视频帧率"] = QVariant::fromValue(QString::number(playListItemResult->getFrameRate()));
    res["视频码率"] = QVariant::fromValue(QString::number(playListItemResult->getBitRate()));
    res["视频流大小"] = QVariant::fromValue(QString::number(playListItemResult->getVideoSize()));
    res["视频尺寸"] = QVariant::fromValue(QString::number(playListItemResult->getVideoWidth())+"*"+QString::number(playListItemResult->getVideoHeight()));
    res["视频编码格式"] = QVariant::fromValue(playListItemResult->getVideoFormat());
    // 音频流参数
    res["音频编码格式"] = QVariant::fromValue(playListItemResult->getAudioFormat());
    res["音频平均码率"] = QVariant::fromValue(QString::number(playListItemResult->getAudioAverageBitRate()));
    res["音频通道数"] = QVariant::fromValue(QString::number(playListItemResult->getChannelNumbers()));
    res["音频采样率"] = QVariant::fromValue(QString::number(playListItemResult->getSampleRate()));
    res["音频流大小"] = QVariant::fromValue(QString::number(playListItemResult->getAudioSize()));
    return res;
}