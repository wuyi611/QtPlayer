/**
 * @file fireworks.hpp
 * @brief QML 视频渲染组件 — 将解码后的视频帧通过 OpenGL 渲染到 QML 界面
 *
 * Fireworks 是 PonyPlayer 视频渲染链的末端。它作为 QQuickItem 嵌入 QML 场景树，
 * 通过 FireworksRenderer（QSGNode）将解码后的 VideoFrameRef 渲染为纹理。
 *
 * 渲染管线:
 *   setVideoFrame() [主线] → update() 触发重绘 → sync() [渲染线程] → init()/render() [渲染线程]
 *
 * 同时还负责:
 *   - 加载并管理 LUT 滤镜（.cube 3D LUT）
 *   - 加载并管理 Shader 滤镜（JSON 片段着色器）
 *   - 视频帧尺寸/帧率/亮度/对比度/饱和度的 QML 属性暴露
 */

#pragma once
#include <QQuickItem>
#include <QObject>
#include <QQuickWindow>
#include <QOpenGLShaderProgram>
#include "renderer.hpp"
#include "platform.hpp"


/**
 * @class Fireworks
 * @brief 视频渲染 QQuickItem，桥接 QML UI 层与 OpenGL 渲染器
 *
 * 继承 QQuickItem，注册为 QML 元素。内部持有 FireworksRenderer（自定义 QSGNode），
 * 通过重写 updatePaintNode() 将其挂载到 Qt Quick 场景图中。
 *
 * 线程模型:
 *   - 构造 / setVideoFrame / Q_PROPERTY getter/setter → 主线（GUI 线程）
 *   - sync / init / render → Qt Quick 渲染线程（通过 DirectConnection 直连）
 */
class Fireworks : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    /// 是否保持原始帧率
    Q_PROPERTY(bool keepFrameRate READ isKeepFrameRate WRITE setKeepFrameRate NOTIFY keepFrameRateChanged)
    /// 当前视频帧高度（像素）
    Q_PROPERTY(int frameHeight READ getHeight NOTIFY frameSizeChanged)
    /// 当前视频帧宽度（像素）
    Q_PROPERTY(int frameWidth READ getWidth NOTIFY frameSizeChanged)
    /// 当前视频帧率（fps）
    Q_PROPERTY(double frameRate READ getFrameRate NOTIFY frameSizeChanged)
    /// 亮度调整（0.0 ~ 2.0，默认 1.0）
    Q_PROPERTY(GLfloat brightness READ getBrightness WRITE setBrightness NOTIFY brightnessChanged)
    /// 对比度调整（0.0 ~ 2.0，默认 1.0）
    Q_PROPERTY(GLfloat contrast READ getContrast WRITE setContrast NOTIFY contrastChanged)
    /// 饱和度调整（0.0 ~ 2.0，默认 1.0）
    Q_PROPERTY(GLfloat saturation READ getSaturation WRITE setSaturation NOTIFY saturationChanged)
    /// 滤镜资源目录路径（只读）
    Q_PROPERTY(QString filterPrefix READ getFilterPrefix)
    /// 已加载的 Shader 滤镜 JSON 列表（只读）
    Q_PROPERTY(QStringList filterJsons READ getFilterJsons)
private:
    /// OpenGL 渲染器节点（自定义 QSGNode，实际渲染在此完成）
    FireworksRenderer *m_renderer;
    /// 滤镜资源目录的绝对路径
    QString m_filterPrefix;
    /// 滤镜目录下所有 .json 滤镜文件的内容列表
    QStringList m_filterJsons;
    /// 当前视频帧高度
    int m_frameHeight = 1;
    /// 当前视频帧宽度
    int m_frameWidth = 1;
    /// 当前视频帧率（= 帧高 / 帧宽，用于宽高比相关计算）
    double m_frameRate = 1.0;
protected:
    /**
     * @brief Qt Quick 场景图更新回调 — 将自定义渲染节点注入场景图
     * @param node 旧的 QSGNode（首次调用时为 nullptr）
     * @param data 更新数据
     * @return 当前渲染节点 m_renderer
     *
     * 每次 QQuickItem::update() 被调用后，在数据同步阶段渲染线程会回调此方法。
     * 直接返回 m_renderer，无需重建节点。
     */
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override {
        return m_renderer;
    }

public:
    /**
     * @brief 构造函数 — 初始化渲染器、加载滤镜、连接渲染信号
     * @param parent 父 QQuickItem
     *
     * 初始化流程:
     *   1. 创建 FireworksRenderer 实例
     *   2. 扫描滤镜目录，加载所有 .json Shader 滤镜
     *   3. 设置 QQuickItem::ItemHasContents 标志（必须有内容才能触发渲染）
     *   4. 监听 windowChanged → 连接 beforeSynchronizing / beforeRendering 信号
     *      （使用 DirectConnection 避免渲染路径中的排队延迟）
     */
    explicit Fireworks(QQuickItem *parent = nullptr): QQuickItem(parent), m_renderer(new FireworksRenderer),
        m_filterPrefix(PonyPlayer::getAssetsDir() + u"/filters"_qs), m_filterJsons() {
        // 扫描滤镜目录，加载所有 .json 片段着色器
        QDir filterDir(m_filterPrefix);
        for(auto && filename : filterDir.entryList({"*.json"})) {
            QFile file = filterDir.filePath(filename);
            file.open(QIODevice::OpenModeFlag::ReadOnly);
            m_filterJsons.append(file.readAll());
            file.close();
        }
        // 标记此 Item 有自定义渲染内容
        this->setFlag(QQuickItem::ItemHasContents);
        // 窗口关联生命周期：绑定窗口时连接渲染信号，解绑时仅告警
        connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win){
            qDebug() << "Window Size Changed:" << static_cast<void *>(win) << ".";
            if (win) {
                // DirectConnection: 在渲染线程直接调用，避免跨线程排队
                // 已读
                connect(this->window(), &QQuickWindow::beforeSynchronizing, m_renderer, &FireworksRenderer::sync, Qt::DirectConnection);
                // 已读
                connect(this->window(), &QQuickWindow::beforeRendering, m_renderer, &FireworksRenderer::init, Qt::DirectConnection);
                win->setColor(Qt::black);
            } else {
                qWarning() << "Window destroy.";
            }

        });
        qDebug() << "Create Hurricane QuickItem.";
    }
    /// 析构 — 释放渲染器所有权（由 QSGNode 树管理生命周期）
    ~Fireworks() override {
        m_renderer = nullptr;
    }

PONY_GUARD_BY(MAIN) private:
    /// @brief 获取滤镜资源目录的绝对路径
    [[nodiscard]] QString getFilterPrefix() const { return m_filterPrefix; }

    /// @brief 是否保持原始帧率（而非跟随显示刷新率）
    [[nodiscard]] bool isKeepFrameRate() const { return m_renderer->isKeepFrameRate(); }

    /// @brief 获取已加载的 Shader 滤镜 JSON 列表
    [[nodiscard]] QStringList getFilterJsons() const { return m_filterJsons; }

    /// @brief 获取当前亮度值
    [[nodiscard]] GLfloat getBrightness() const { return m_renderer->getBrightness(); }

    /// @brief 设置是否保持原始帧率
    void setKeepFrameRate(bool keep) {
        m_renderer->setKeepFrameRate(keep);
        emit keepFrameRateChanged();
    }

    /// @brief 设置亮度（0.0 ~ 2.0）
    void setBrightness(GLfloat b) {
        m_renderer->setBrightness(b);
        emit brightnessChanged();
    }

    /// @brief 获取当前对比度值
    [[nodiscard]] GLfloat getContrast() const {
        return m_renderer->getContrast();
    }

    /// @brief 设置对比度（0.0 ~ 2.0）
    void setContrast(GLfloat c) {
        m_renderer->setContrast(c);
        emit contrastChanged();
    };

    /// @brief 获取当前饱和度值
    [[nodiscard]] GLfloat getSaturation() const { return m_renderer->getSaturation(); };

    /// @brief 设置饱和度（0.0 ~ 2.0）
    void setSaturation(GLfloat s) {
        m_renderer->setSaturation(s);
        emit saturationChanged();
    };

    /// @brief 获取当前视频帧高度
    [[nodiscard]] int getHeight() const {
        return m_frameHeight;
    }

    /// @brief 获取当前视频帧宽度
    [[nodiscard]] int getWidth() const {
        return m_frameWidth;
    }

    /// @brief 获取当前视频帧率
    [[nodiscard]] double getFrameRate() const {
        return m_frameRate;
    }


public slots:

    /**
     * @brief 接收解码后的视频帧并触发渲染
     * @param pic 视频帧引用（VideoFrameRef，零拷贝共享内存）
     *
     * 必须在 GUI 线程调用。调用链: setVideoFrame → update() → sync → render
     *
     * 帧尺寸变化时自动更新 m_frameWidth/m_frameHeight/m_frameRate 并发射 frameSizeChanged 信号。
     * 如果帧与当前帧尺寸相同（isSameSize），则跳过尺寸更新，仅触发重绘。
     */
    void setVideoFrame(const VideoFrameRef &pic) {
        // 必须由 GUI 线程调用
        // 调用链: setImage → sync → render
        // 帧数据可能正在渲染线程中使用，此处不可释放
        // 帧无变化时立即返回
        if (m_renderer->setVideoFrame(pic)) {
            // 标记脏区，触发 Qt Quick 重绘
            this->update();
            if (!pic.isSameSize(m_frameWidth, m_frameHeight)) {
                m_frameWidth = pic.getWidth();
                m_frameHeight = pic.getHeight();
                // frameRate 在此处表示帧宽高比，用于渲染时的比例计算
                m_frameRate = static_cast<double>(m_frameHeight) / static_cast<double>(m_frameWidth);
                // hurricane发出的frameSizeChanged信号什么也不会触发
                emit frameSizeChanged();
            }
        }
    }

    /**
     * @brief 设置 LUT 滤镜（3D LUT 颜色查找表）
     * @param path 滤镜文件名（相对于 filterPrefix 目录）
     *
     * 从滤镜目录加载指定图片作为颜色查找表，传给渲染器。
     * 传入空字符串则清除当前 LUT 滤镜。
     * 图片自动转换为 RGB888 格式供 GPU 使用。
     */
    Q_INVOKABLE void setLUTFilter(const QString& path) {
        QImage image;
        if (!path.isEmpty()) {
            image.load(QDir(m_filterPrefix).filePath(path));
            image.convertTo(QImage::Format_RGB888);
        }

        m_renderer->setLUTFilter(image);
    }



signals:
    /// 亮度值发生变化
    void brightnessChanged();

    /// 对比度值发生变化
    void contrastChanged();

    /// 饱和度值发生变化
    void saturationChanged();

    /// 视频帧尺寸或帧率发生变化
    void frameSizeChanged();

    /// 保持帧率设置发生变化
    void keepFrameRateChanged();
};





