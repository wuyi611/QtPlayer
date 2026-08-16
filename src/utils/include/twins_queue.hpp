/**
 * @file twins_queue.hpp
 * @brief 联动阻塞队列 (TwinsBlockQueue) — 单生产者 / 多队列 / 单消费者的线程安全缓冲
 *
 * 设计要点:
 *   - 队列对 (音频/视频) 共享同一把互斥锁、条件变量与开关标志,
 *     因此关闭或清空任意一个都会同时影响其孪生队列.
 *   - 消费者 (viewFront / remove / skip) 在队列为空时阻塞等待;
 *     生产者 (push) 在音频、视频两个队列都达到容量上限时阻塞.
 *   - 用 nullptr 作为 EOF 哨兵: 解码循环读到文件末尾时 push(nullptr),
 *     消费者通过 remove(protectNull=true) 将其视为"队列已结束"而返回空帧.
 */
//
// Created by ColorsWind on 2022/5/4.
//
#pragma once
#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>
#include <utility>

//#define DEBUG_PRINT_FUNCTION_CALL  // (调试用) 打印入队/出队/清空日志
/**
 * @brief 联动队列. 用于单生产者多队列, 单消费者通信.
 *
 * 每个队列可通过 twins() 生成一个"孪生"队列, 二者共用同一把锁 / 条件变量 /
 * 打开标志. 典型用法: 音频队列为主, 视频队列为其孪生, 解码线程同时向两者
 * push 帧, 视频帧与音频帧的消费互相独立阻塞.
 *
 * 阻塞与唤醒约定:
 *   - 无数据时, viewFront / remove / skip 阻塞在条件变量上;
 *   - 队列关闭 (close) 或禁用 (setEnable(false)) 会唤醒所有等待线程.
 *
 * @tparam T 队列元素类型 (PonyPlayer 中为 AVFrame*)
 */
template<typename T>
class TwinsBlockQueue {
    /// 底层 FIFO 缓冲
    std::queue<T> m_data;
    /// 本队列的启用开关 (与共享的 m_open 叠加后判定 isOpen)
    bool m_enable{true};
    /// 队列名称 (仅用于日志)
    const std::string m_name;
    /// 期望容量 (软上限): 队列内元素少于该值时 push 不会被阻塞
    const size_t m_prefer;

    /// 孪生队列指针 (生成孪生后二者互指; 未生成时指向自己)
    TwinsBlockQueue<T> *m_twins;
    /// 共享互斥锁 (由主队列创建, 孪生队列共用)
    std::mutex *m_mutex = nullptr;
    /// 共享条件变量 (由主队列创建, 孪生队列共用)
    std::condition_variable *m_cond = nullptr;
    /// 共享"队列打开"标志 (由主队列创建, 孪生队列共用)
    bool *m_open  = nullptr;
private:
    /**
     * @brief 私有构造函数 — 仅由 twins() 调用, 创建与主队列共享同步原语的孪生队列
     * @param name 孪生队列名称
     * @param prefer 孪生队列的期望容量
     * @param twins 主队列 (提供锁 / 条件变量 / 打开标志)
     */
    TwinsBlockQueue(
            std::string name,
            size_t prefer,
            TwinsBlockQueue<T> *twins
    ) : m_name(std::move(name)), m_prefer(prefer), m_twins(twins){
        if (prefer < 2) { throw std::runtime_error("PreferSize must not less than 2."); }
        this->m_mutex = twins->m_mutex;
        this->m_cond = twins->m_cond;
        this->m_open = twins->m_open;
    }

    /// 本队列是否可读写: 共享打开标志 && 本队列启用
    inline bool isOpen() { return *m_open && m_enable; }

public:
    /**
     * @brief 创建主队列, 并分配共享的同步原语 (锁 / 条件变量 / 打开标志)
     * @param name 队列名称 (日志用)
     * @param prefer 期望容量, 必须 >= 2
     * @throw std::runtime_error prefer < 2 时抛出
     */
    TwinsBlockQueue(std::string name, size_t prefer) : m_name(std::move(name)), m_prefer(prefer) {
        if (prefer < 2) { throw std::runtime_error("PreferSize must not less than 2."); }
        this->m_mutex = new std::mutex;
        this->m_cond = new std::condition_variable;
        this->m_open = new bool{true};
        this->m_twins = this;
    }

    /// 析构 — 先关闭队列唤醒所有阻塞线程, 再由主队列释放共享同步原语
    ~TwinsBlockQueue() {
        close();
    }

    /**
     * @brief 生成孪生队列 — 与当前队列共享锁 / 条件变量 / 打开标志
     * @param name 孪生队列名称
     * @param prefer 孪生队列期望容量
     * @return 新创建的孪生队列指针 (由调用方负责释放)
     * @throw std::runtime_error 已存在孪生队列时抛出 (每个队列最多一个孪生)
     */
    TwinsBlockQueue<T> *twins(const std::string &name, size_t prefer) {
        std::unique_lock lock(*m_mutex);
        if (m_twins != this) { throw std::runtime_error("Already generate twins."); }
        m_twins = new TwinsBlockQueue<T>{name, prefer, this};
        return m_twins;
    }

    /**
     * @brief 启用/禁用本队列 (只影响自身, 不影响孪生)
     * @param b true 启用; false 禁用并唤醒所有等待线程
     *
     * 禁用后 isOpen() 为假, push / remove 将立即返回而不阻塞.
     */
    void setEnable(bool b) {
        std::unique_lock lock(*m_mutex);
        m_enable = b;
        if (!m_enable)
            m_cond->notify_all();
    }

    /// 查询本队列是否启用
    [[nodiscard]] bool isEnable() const {
        std::unique_lock lock(*m_mutex);
        return m_enable;
    }


    /**
     * @brief 关闭队列 — 置共享打开标志为 false 并唤醒所有等待线程
     *
     * 关闭后: push 返回 false 拒绝入队; 消费者立刻从阻塞中返回,
     * 空队列时返回默认值 (解码层以 nullptr 空指针表示 EOF).
     * 由于孪生队列共享该标志, 关闭一个即同时关闭另一个.
     */
    void close() {
        std::unique_lock lock(*m_mutex);
        *m_open = false;
        m_cond->notify_all();
#ifdef DEBUG_PRINT_FUNCTION_CALL
        qDebug() << m_name.c_str() << "Close" << m_data.size();
#endif
    }

    /**
     * @brief 清空队列中的全部元素, 并对每个元素调用 freeFunc 释放资源
     * @param freeFunc 元素释放回调 (通常为 av_frame_free)
     *
     * 清空后通知所有等待线程 (唤醒可能因队列满而阻塞的生产者).
     */
    void clear(const std::function<void(T)> &freeFunc) {
        std::unique_lock lock(*m_mutex);
        while(!m_data.empty()) {
            freeFunc(m_data.front());
            m_data.pop();
        }
        m_cond->notify_all();
#ifdef DEBUG_PRINT_FUNCTION_CALL
        qDebug() << m_name.c_str() << "Clear" << m_data.size();
#endif
    }

    /**
     * @brief 重新打开队列 — 置共享打开标志为 true
     *
     * 与 close 配对使用: 暂停解码时 close, 恢复解码时 open.
     */
    void open() {
        std::unique_lock lock(*m_mutex);
        *m_open = true;
    }


    /**
     * @brief 入队一个元素; 队列未打开时直接失败
     * @param item 待入队元素
     * @return true 入队成功; false 队列未打开
     *
     * 阻塞规则: 仅当本队列与孪生队列都达到容量上限时才阻塞等待,
     * 即音频/视频两个队列合计缓冲被软限制在 m_prefer + twins->m_prefer 左右.
     * 队列从空变为非空时唤醒等待的消费者 (避免每次入队都惊群).
     */
    bool push(const T item) {
        std::unique_lock lock(*m_mutex);
        if (!isOpen()) {
            return false;
        }
        m_cond->wait(lock, [this]{
            return this->m_data.size() < m_prefer ||
                    (m_twins->m_enable && m_twins->m_data.size() < m_twins->m_prefer) || !isOpen();
        });
        m_data.push(item);
        // 唤醒所有等待
        if (m_data.size() == 1) { m_cond->notify_all(); }
#ifdef DEBUG_PRINT_FUNCTION_CALL
        qDebug() << m_name.c_str() << "Push" << m_data.size();
#endif
        return true;
    }

    /**
     * @brief 查看队首元素并应用变换函数, 不弹出元素 (阻塞直到有数据或队列关闭)
     * @tparam R 变换结果类型
     * @param func 变换函数, 输入为队首元素; 队列空时输入默认构造值
     * @return func 的返回值
     */
    template<typename R>
    R viewFront(const std::function<R(T)> &func) {
        const static T defaultValue = {};
        std::unique_lock lock(*m_mutex);
        m_cond->wait(lock, [this]{ return !this->m_data.empty() || !isOpen();});
        if (m_data.empty()) {
            return func(defaultValue);
        } else {
            return func(m_data.front());
        }
    }

    /**
     * @brief 弹出队首元素 (阻塞直到有数据或队列关闭)
     * @param protectNull true 时, 若队首为 nullptr (EOF 哨兵) 则不弹出并返回空值
     * @return 队首元素; 队列为空或命中 EOF 保护时返回默认构造值
     *
     * 队列弹空到期望容量的一半以下时通知生产者补充 (反压, 避免抖晃).
     */
    T remove(bool protectNull) {
        std::unique_lock lock(*m_mutex);
        m_cond->wait(lock, [this]{ return !this->m_data.empty() || !isOpen();});
        if (m_data.empty()) {
            return {};
        } else {
            T ret = m_data.front();
            if (protectNull && !ret) { return {}; }
            m_data.pop();
            if (m_data.size() < m_prefer / 2 && isOpen()) { this->m_cond->notify_all(); }
            return ret;
        }
    }

    /**
     * @brief 跳过队首满足 predicate 条件的元素, 并对被跳过的元素调用 freeFunc
     * @param predicate 判断条件 (返回 true 表示跳过)
     * @param freeFunc 元素释放回调
     * @return 实际跳过的元素个数
     *
     * 阻塞直到有数据或队列关闭; 遇到不满足条件的队首元素或空队列即停止.
     */
    int skip(const std::function<bool(T)> &predicate, const std::function<void(T)> &freeFunc) {
        int ret = 0;
        while(true) {
            std::unique_lock lock(*m_mutex);
            m_cond->wait(lock, [this]{ return !this->m_data.empty() || !isOpen();});
            if (m_data.empty()) { return ret;}
            T element = m_data.front();
            if (element && predicate(element)) {
                m_data.pop();
                lock.unlock();
                freeFunc(element);
                ++ret;
            } else break;
        }
        return ret;
    }

// ===== 以下为遗留的旧接口 (已注释, 当前未使用) =====
//    /**
//     * 取出队首元素, 若缺少元素, 则阻塞直到有元素.
//     * @return
//     */
//    const T& front() {
//        const static T defaultValue = {};
//        std::unique_lock lock(*m_mutex);
//        m_cond->wait(lock, [this]{ return !this->m_data.empty() || !isOpen();});
//#ifdef DEBUG_PRINT_FUNCTION_CALL
//        qDebug() << m_name.c_str() << "IsEmpty" << m_data.empty() << "isOpen" << isOpen();
//#endif
//        if (m_data.empty()) {
//            return defaultValue;
//        } else {
//            return m_data.front();
//        }
//    }
//
////    /**
////     * 删除队首元素并返回, 需要保证 size >= 1.
////     */
////    T remove() {
////        std::unique_lock lock(mutex);
////        if (this->data.empty()) {
////            cond.wait(lock);
////        }
////        auto ret = data.front();
////        data.pop();
////
////        return ret;
////    }
//
//    /**
//     * 删除队首元素, 需要保证 size >= 1.
//     */
//    bool pop() {
//        std::unique_lock lock(*m_mutex);
//        if (m_data.empty()) { return false; }
//#ifdef QT_DEBUG
//        // nullptr signals end of file
//        if (!m_data.front()) { throw std::runtime_error("Should not pop nullptr"); }
//#endif
//        m_data.pop();
//        // avoid bumpy
//        if (m_data.size() < m_prefer / 2) { this->m_cond->notify_all(); }
//#ifdef DEBUG_PRINT_FUNCTION_CALL
//        qDebug() << m_name.c_str() << "Pop" << m_data.size();
//#endif
//        return *m_open;
//    }
//


};

/// 显式实例化 AVFrame* 特化, 避免模板在多个翻译单元重复展开
template class TwinsBlockQueue<AVFrame *>;
