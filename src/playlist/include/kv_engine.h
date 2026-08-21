#pragma once

#include <QtCore>
#include <QString>
#include <QtSql/QSqlDatabase>
#include <QDebug>
#include <QDir>
#include <unordered_map>
#include <QUuid>

/**
 * @brief PonyKVConnect 基于 SQLite 的键值存取引擎
 *
 * 负责与底层 SQLite 数据库交互：根据 QObject 的元对象(metaObject)属性动态建表、
 * 插入/删除/查询对象，实现了“对象属性 ↔ 数据库表列”的映射。
 */
class PonyKVConnect {
private:
    QSqlDatabase db;  // SQLite 数据库连接
public:
    /**
     * @brief 构造连接，并确保数据目录存在
     * @param dbName 数据库文件名（位于 PONYPATH/data 下）
     */
    explicit PonyKVConnect(const QString &dbName);

    // 判断指定表是否已存在
    bool hasTable(const QString &tableName);

    // 根据类名动态创建对应表，列取自类属性，主键为 _uuid_
    void createTableFrom(const QString &className, const QString &tableName);

    // 将 Qt 属性类型(QString/int/float/QDir)映射为 SQL 的 DDL 类型
    static QString qTypeToDDL(const QString &qType);

    // 向表中插入一个对象（按其属性值生成 INSERT 语句）
    void insert(const QString &tableName, const QObject *object);

    // 按对象 _uuid_ 从表中移除该对象
    void remove(const QString &tableName, const QObject *object);

    // 按指定键值对从表中移除记录，并删除对应的预览图文件
    void removeByKV(const QString &tableName, const QString &key, const QString &value);

    // 按指定键值查询，返回匹配的第一个对象（泛型）
    template<typename T>
    T* search(const QString &tableName, const QString &className, const QString &key, const QString &value);

    // 取出表中全部记录，返回 QObject 指针列表
    QList<QObject *> retrieveData(const QString &tableName, const QString &className);

    // 取出表中全部记录，并转换为指定类型 T 的指针列表（泛型）
    template<typename T>
    QList<T *> retrieveDataByClass(const QString &tableName, const QString &className);
};

/**
 * @brief ListItem 所有条目对象的基类
 *
 * 提供唯一的 _uuid_ 字段（UUID 字符串）作为数据库主键，
 * 并通过 Q_PROPERTY 暴露给 Qt 元对象系统，以便被 PonyKVConnect 序列化。
 */
class ListItem : public QObject {
    Q_OBJECT
    // 主键属性：唯一标识符，读写均映射到 _uuid_ 成员
    Q_PROPERTY(QString _uuid_ READ getUUID WRITE setUUID)
protected:
    QString _uuid_;
public:
    // 构造时自动生成一个 UUID 作为该条目的唯一标识
    Q_INVOKABLE ListItem() { _uuid_ = QUuid::createUuid().toString(); };

    // 设置 UUID
    Q_INVOKABLE void setUUID(QString uuid) { _uuid_ = std::move(uuid); };

    // 获取 UUID
    Q_INVOKABLE QString getUUID() { return _uuid_; };

    // 虚析构（Qt 对象树会自动释放）
    Q_INVOKABLE ~ListItem() override = default;

//    Q_INVOKABLE ListItem(const ListItem &listItem) : QObject() { _uuid_ = listItem._uuid_; };
};

/**
 * @brief PonyKVList 面向某张表的类型化增删查封装
 *
 * 将某一种具体类型 T 的对象与一张数据库表绑定，提供：
 * 插入、按键删除、按键查询单条、取出全部等操作，并维护内存中的数据缓存。
 *
 * @tparam T 条目类型（如 PlayListItem），需继承自 ListItem/QObject
 */
template<typename T>
class PonyKVList {
private:
    PonyKVConnect engine;   // 底层数据库引擎
    QString dbName;         // 数据库名
    QString tableName;      // 对应的表名
    QString className;      // 对象类型名（用于反射建表/查询）
    QList<T *> data;        // 内存中的条目缓存
    T* infoData;            // 当前查询到的单条信息（预留）
public:
    // 构造：建立数据库连接，若表不存在则自动建表，并加载已有数据
    PonyKVList(QString _dbName, QString _tableName, QString _className);

    // 获取表名
    QString getTableName() { return tableName; }

    // 插入一条记录（写入数据库并加入内存缓存）
    void insert(T *item);

    // 按指定键值删除记录
    void remove(const QString& key,const QString& value);

    // 按指定键值查询单条记录
    T* extractInfo(QString key,QString value);

    // 取出内存中缓存的全部记录
    QList<T*> extract();

};