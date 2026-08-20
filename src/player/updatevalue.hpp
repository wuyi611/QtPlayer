//
// Created by ColorsWind on 2022/5/20.
//
#pragma once
#include <utility>
#include "frame.hpp"

/**
 * @brief 带"脏标记"的值包装器 — 跨线程"生产者 → 消费者"最新值同步的通用载体
 *
 * 设计动机 (配合 RenderSettings 使用):
 *   - GUI 线程 (生产者) 通过 operator= 写入新值, 同时置位脏标记;
 *   - 渲染线程 (消费者) 通过 getUpdate() 取走最新值并清除脏标记 (一次性消费),
 *     用 isUpdate() 判断"自上次读取后是否发生变化", 从而只重做必要的工作
 *     (例如亮度/对比度等 uniform 值只在变化时才重新上传, 见 renderer.hpp)。
 *
 * 读取语义:
 *   - getUpdate()   : 读取最新值 **并清除脏标记** (一次性消费);
 *   - operator T&() : 只读当前值, **不**清除脏标记 (可重复查询)。
 *
 * 同步语义:
 *   - updateBy() 把另一份实例的修改合并进当前实例: 按位或合并脏标记,
 *     并取走对方的最新值 (同时清除对方标记), 供多份副本之间的增量同步。
 */
template<typename T>
class UpdateValue {
protected:
    T m_value;        ///< 当前值
    bool m_update;    ///< 脏标记: 自上次 getUpdate() 后是否被重新赋值
public:

    /// 默认构造 — 值默认初始化, 标记为"需要更新"(保证首次读取必然生效)
    UpdateValue() : m_value(), m_update(true) {}

    /// 构造 — 用给定值初始化, 标记为"需要更新"
    explicit UpdateValue(T value) : m_value(std::move(value)), m_update(true) {}


    /**
     * @brief 隐式转换 — 只读当前值, 不清除脏标记
     * @note 用于"查询但不消费"的场景; 刻意不标记 explicit,
     *       使 UpdateValue<T> 能像 T 一样直接参与表达式运算
     */
    operator const T&() const { // NOLINT(google-explicit-constructor)
        return m_value;
    }

    /**
     * @brief 读取最新值并清除脏标记 (一次性消费)
     * @return 当前值; 此后 isUpdate() 返回 false, 直到被重新赋值
     */
    virtual const T& getUpdate() {
        m_update = false;
        return m_value;
    }

    /**
     * @brief 赋值 — 更新值并置位脏标记 (生产端入口)
     * @param v 新值
     * @return 自身引用 (支持链式赋值)
     */
    virtual UpdateValue& operator=(const T &v) {
        this->m_value = v;
        m_update = true;
        return *this;
    }

    /**
     * @brief 合并另一份实例的修改到当前实例 (多副本增量同步)
     * @param updateValue 来源实例 (其脏标记会被清除)
     * @note 脏标记按位或合并: 任一方标记过更新, 本实例也标记为需要更新;
     *       取走对方最新值的同时消费掉对方的更新
     */
    void updateBy(UpdateValue& updateValue) {
        this->m_update |= updateValue.m_update;
        this->m_value = updateValue.getUpdate();
    }

    /// @return 自上次 getUpdate() 后值是否被更新过
    [[nodiscard]] bool isUpdate() const {
        return m_update;
    }
};

/**
 * @brief UpdateValue<VideoFrameRef> 的特化 — 视频帧 + "尺寸是否变化"标记
 *
 * 在通用脏标记之外, 额外跟踪帧尺寸是否发生变化 (m_updateSize):
 *   - 尺寸变化时, 渲染器需要重新分配纹理;
 *   - 尺寸不变时, 渲染器只需把新像素数据上传到既有纹理 (局部更新更高效)。
 * 见 renderer.hpp 中 isUpdateSize() 的调用点; 注意 operator= 中的实际实现
 * 与上述预期存在出入, 详见其注释。
 */
class UpdateValueVideoFrameRef : public UpdateValue<VideoFrameRef> {
private:
    bool m_updateSize;    ///< 尺寸脏标记 (预期语义: 自上次消费后帧尺寸是否变化; 实际置位逻辑见 operator= 的 ⚠ 说明)
public:
    /// 默认构造 — 空视频帧, 标记为"需要更新"且"尺寸已变化"
    UpdateValueVideoFrameRef() : UpdateValue(VideoFrameRef{}), m_updateSize(true) {}

    /// 构造 — 用给定视频帧初始化, 标记为"需要更新"且"尺寸已变化"
    explicit UpdateValueVideoFrameRef(const VideoFrameRef &value) : UpdateValue(value), m_updateSize(true) {}



    /**
     * @brief 赋值 — 更新帧并置位"内容更新"标记 (生产端入口)
     *
     * 实际行为:
     *   - 新帧与当前帧指向同一底层帧 (operator==) 时直接返回, 不产生任何更新;
     *   - 否则更新 m_value 并置位 m_update (内容更新);
     *   - 若新帧与当前帧**尺寸相同** (isSameSize), 同时把 m_updateSize 置为 true;
     *     尺寸不同时 m_updateSize 保持原值。
     *
     * ⚠ 与渲染端的语义对照: 渲染端 (renderer.hpp:377) 把 isUpdateSize() 为 true
     *   解释为"帧尺寸变化, 需要整体重传纹理", 而这里恰恰在尺寸**相同**时置位该标记,
     *   二者方向相反, 疑似笔误 (条件可能应为 !isSameSize);(已更改)
     *   在确认语义前, 请勿依赖该标记的精确取值。
     */
    UpdateValueVideoFrameRef& operator=(const VideoFrameRef &videoFrame) override {
        if (videoFrame == this->m_value) { return *this; }
        if (!m_value.isSameSize(videoFrame)) { this->m_updateSize = true; }
        this->m_value = videoFrame;
        m_update = true;
        return *this;
    }

    /**
     * @brief 读取最新帧并清除全部脏标记 (一次性消费)
     * @return 最新视频帧; 同时清除"内容更新"与"尺寸更新"标记
     */
    const VideoFrameRef& getUpdate() override {
        m_update = false;
        m_updateSize = false;
        return m_value;
    }

    /**
     * @brief 合并另一份实例的修改到当前实例 (多副本增量同步)
     * @param updateValue 来源实例 (其脏标记会被清除)
     * @note 内容 / 尺寸两组脏标记各自按位或合并, 并取走对方最新帧
     */
    void updateBy(UpdateValueVideoFrameRef& updateValue) {
        this->m_update |= updateValue.m_update;
        this->m_updateSize |= updateValue.m_updateSize;
        this->m_value = updateValue.getUpdate();
    }

    /// @return 帧尺寸是否发生变化 (预期语义: 渲染器据此决定是否重分配纹理; 实际取值见 operator= 的 ⚠ 说明)
    bool isUpdateSize() const {
        return m_updateSize;
    }
};
