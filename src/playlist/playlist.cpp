// ============================================================
// playlist.cpp —— 播放列表后端逻辑的实现
// 实现 PlayListItem 的存取方法，以及 PlayList 的增删查、提取、取信息等操作。
// ============================================================

#include "playlist.h"

// 获取文件名
QString PlayListItem::getFileName() {
    return fileName;
}

// 获取目录字符串（QDir::path() 返回目录的本地路径）
QString PlayListItem::getDirectory() {
    return dir.path();
}

//PlayListItem::PlayListItem(const PlayListItem &item) : ListItem(item) {
//    fileName = item.fileName;
//    dir = item.dir;
//}

// PlayList 构造函数：用指定的数据库名、表名、类名初始化底层存取对象
PlayList::PlayList(QString _dbName, QString _tableName, QString _className):pkvList(_dbName, _tableName, _className) {
    qDebug()<<"PlayList init!\n";
}

//PlayList::QQmlListProperty<QObject*> playListItems() {
//
//}
//
//void PlayList::appendPlayList(QObject *qobj) {
//    data.append(qobj);
//}
//
//int PlayList::playListCount() const {
//    return data.count();
//}
//
//QObject* PlayList::listItem(int index) const {
//    return data.at(index);
//}
//
//void PlayList::clearPlayList() {
//    data.clear();
//}
//
//void PlayList::appendPlayList(QQmlListProperty<QObject*>* list,QObject *qobj) {
//    reinterpret_cast<QObject*>(list->data)->appendPlayList(qobj);
//}
//
//int PlayList::playListCount(QQmlListProperty<QObject*>* list) const {
//    return reinterpret_cast<QObject*>(list->data)->playListCount();
//}
//
//QObject* PlayList::listItem(QQmlListProperty<QObject*>* list,int i) const {
//    return reinterpret_cast<QObject*>(list->data)->listItem(i);
//}
//
//void PlayList::clearPlayList(QQmlListProperty<QObject*>* list) {
//    reinterpret_cast<QObject*>(list->data)->clearPlayList();
//}

//PlayList::PlayList(const QString &name) {
//    qRegisterMetaType<PlayListItem *>("PlayListItem");
//
//    PonyKVList<PlayListItem> list(name, QString("playlist"), QString("PlayListItem"));
//    auto *item = new PlayListItem("233", QDir("/usr/bin"));
//
//    list.insert(item);
//
////    qDebug() << data[0]->property("dir");
//}

// 插入一个媒体条目到播放列表
void PlayList::insert(PlayListItem *item){
    qDebug()<<"PlayList is going to insert a PlayListItem.";
    if(!item)
        qDebug()<<"But this item is NULL which means it was not received correctly.";
    else
        qDebug()<<"And this item was received correctly.";
    pkvList.insert(item);
    qDebug()<<"insert to db done!";
}

// 按文件路径删除播放列表中的对应条目
void PlayList::remove(QString filepath){
    qDebug()<<"remove";
    pkvList.remove("path",filepath);
    qDebug()<<"remove "<<filepath<<" done!";
}

// 按主键搜索条目（当前为占位实现，恒返回 nullptr）
PlayListItem* PlayList::search(QString key){
    qDebug()<<"search";
    return NULL;
}

// 按路径查询媒体的详细信息，并发出 getInfoDone 信号回传结果
void PlayList::getInfo(QString path) {
    qDebug()<<"getInfo";
    PlayListItem* res = pkvList.extractInfo("path",path);
    emit getInfoDone(res);
}

/*
 * 提取数据库信息并处理为 simpleListItem 对象，然后向 Controller 发送处理完毕信号
 */
// 提取数据库中的全部条目，并转换为 simpleListItem 后通过 extractDone 信号发回
void PlayList::extractAndProcess() {
    // 取出全部 PlayListItem
    QList<PlayListItem*> rce = pkvList.extract();

    // 转换为左侧列表所需的简单条目对象
    QList<simpleListItem*> res;
    simpleListItem* slItem;
    foreach(PlayListItem* item, rce) {
        slItem = new simpleListItem(item->getFileName(),item->getPath(),item->getIconPath());
        res.append(slItem);
    }
    emit extractDone(res);
}