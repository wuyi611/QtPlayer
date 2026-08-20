/**
 * @file renderer.hpp
 * @brief OpenGL 视频渲染器 — 将解码出的 YUV 视频帧渲染进 Qt Quick 场景图
 *
 * 本文件实现了 FireworksRenderer（QSGRenderNode 子类），
 * 是 PonyPlayer 渲染管线在 GPU 侧的落地点。
 *
 * 渲染管线:
 *   解码线程 →(VideoFrameRef 跨线程传递)→ GUI 线程 Fireworks::setVideoFrame
 *   → QQuickItem::update() → [渲染线程] beforeSynchronizing::sync()
 *   → beforeRendering::init() / render()
 *
 * 线程模型:
 *   - mainSettings   : 仅 GUI 线程读写 (QML 属性、setVideoFrame / setLUTFilter)
 *   - renderSettings : 仅渲染线程读写 (sync() 阶段由 mainSettings 同步而来)
 *   - sync() 在渲染线程执行, 恰逢 GUI 线程与渲染线程都被阻塞,
 *     因此两套设置的读写不存在数据竞争, 内存可见性也有保证.
 */
//
// Created by ColorsWind on 2022/5/7.
//
#pragma once
#ifdef __APPLE__
// 在 macOS 上抑制 OpenGL 弃用警告
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#undef GL_SILENCE_DEPRECATION
#endif

// Qt Quick 场景图与 OpenGL 相关头文件
#include <QQuickItem>
#include <QQuickWindow>
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QQuickFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLDebugLogger>
#include <QVector>
#include <QSGSimpleTextureNode>
#include <QSGRenderNode>
#include <utility>
#include "updatevalue.hpp" // UpdateValue: 带"脏标记"的跨线程值同步
#include "frame.hpp"       // VideoFrameRef: 引用计数的视频帧句柄

// glVertexAttribPointer 等函数传入的空指针偏移 (表示"从缓冲开头读取")
const static void* ZERO_OFFSET = nullptr;
// 全屏矩形的顶点数据, 每个顶点 5 个分量: (x, y, z, u, v)
// 顺序为右上、右下、左上、左下; 纹理坐标令图像正立显示
const static GLfloat VERTEX_POS[] = {
        1, 1, 0, 1, 0,      // 右上角: 位置 (1,1,0), 纹理坐标 (1,0)
        1, 0, 0, 1, 1,      // 右下角: 位置 (1,0,0), 纹理坐标 (1,1)
        0, 1, 0, 0, 0, // 左上角: 位置 (0,1,0), 纹理坐标 (0,0)
        0, 0, 0, 0, 1, // 左下角: 位置 (0,0,0), 纹理坐标 (0,1)
};
// 由两个三角形拼成矩形所需的索引 (0-1-2 与 1-2-3)
const static GLuint VERTEX_INDEX[] = {
        0, 1, 2,
        1, 2, 3,
};

/**
 * @brief 渲染参数集合 — GUI 线程与渲染线程之间传递的设置
 *
 * 维护两份实例:
 *   - mainSettings   : 由 GUI 线程修改 (QML 属性 / 新视频帧 / LUT 滤镜)
 *   - renderSettings : 由渲染线程使用 (render() 读取)
 *
 * 每次渲染前, sync() 在渲染线程调用 updateBy(),
 * 将 GUI 线程的修改合并进渲染线程副本。
 * 所有字段都基于 UpdateValue, 自带"自上次读取后是否变化"的脏标记,
 * 渲染器据此决定纹理是需要整体重传还是仅局部更新。
 */
struct RenderSettings {
    // 为保证内存可见性, 在 GUI 线程与渲染线程都被阻塞的时机同步。
    // 存在两份 RenderSettings: 一份可在 GUI 线程修改 (如被 QML 修改),
    // 另一份在 SYNC 阶段被同步到渲染线程。
    UpdateValueVideoFrameRef videoFrame;                // 待渲染的视频帧 (附带"尺寸是否变化"标记)
    UpdateValue<GLfloat> brightness{0.0F};      // 亮度调整
    UpdateValue<GLfloat> contrast{1.0F};        // 对比度调整
    UpdateValue<GLfloat> saturation{1.0F};      // 饱和度调整
    UpdateValue<QImage> lutFilter;                  // LUT 滤镜图像 (空图像表示无滤镜)
    UpdateValue<bool> keepFrameRate{true};  // 是否保持原始宽高比渲染 (letterbox)

    /**
     * @brief 将另一份设置的修改合并到当前实例
     * @param settings 来源设置 (通常是 GUI 线程的 mainSettings)
     * @note 每个字段按位或合并脏标记, 并取走对方的最新值
     */
    void updateBy(RenderSettings &settings) {
        videoFrame.updateBy(settings.videoFrame);
        brightness.updateBy(settings.brightness);
        contrast.updateBy(settings.contrast);
        saturation.updateBy(settings.saturation);
        lutFilter.updateBy(settings.lutFilter);
        keepFrameRate.updateBy(settings.keepFrameRate);
    }

};

/**
 * @class FireworksRenderer
 * @brief QSGRenderNode 渲染节点 — 在渲染线程上执行真正的 OpenGL 绘制
 *
 * 为什么需要与 QQuickItem 分离的对象:
 *   - QQuickItem 的生命周期在 GUI 线程;
 *   - 实际渲染可能发生在渲染线程, 而渲染期间 GUI 线程可能销毁 QuickItem;
 *   - 因此把"渲染状态"与"GUI 状态"拆成两个对象, 通过 sync() 安全同步。
 *
 * 职责:
 *   - 编译并链接着色器 (YUV→RGB 转换 + LUT 滤镜 + 亮度/对比度/饱和度调整)
 *   - 创建 VAO/VBO/EBO 与 Y/U/V/LUT 四张纹理
 *   - 每帧按需更新纹理内容, 绘制全屏矩形
 *
 * 生命周期回调 (均发生在渲染线程):
 *   - beforeSynchronizing → sync(): 同步 GUI 线程的设置
 *   - beforeRendering     → init() : 首次调用时初始化 GL 资源
 *   - 场景图渲染          → render(): 绘制一帧
 */
class FireworksRenderer : public QObject, public QSGRenderNode, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
    // 暴露给 QML 的调节属性 (均读写 mainSettings, 因此只能在 GUI 线程访问)
    Q_PROPERTY(GLfloat brightness READ getBrightness WRITE setBrightness)
    Q_PROPERTY(GLfloat contrast READ getContrast WRITE setContrast)
    Q_PROPERTY(GLfloat saturation READ getSaturation WRITE setSaturation)
    Q_PROPERTY(bool keepFrameRate READ isKeepFrameRate WRITE setKeepFrameRate)
    friend class Fireworks;

PONY_GUARD_BY(MAIN) private:
    // properties —— 以下属性访问器仅供 GUI 线程调用

    /// @return 是否保持原始宽高比渲染
    [[nodiscard]] bool isKeepFrameRate() const { return mainSettings.keepFrameRate; }

    /// @brief 设置是否保持原始宽高比渲染
    void setKeepFrameRate(bool keep) { mainSettings.keepFrameRate = keep; }

    /// @return 当前亮度值
    [[nodiscard]] GLfloat getBrightness() const { return mainSettings.brightness; }

    /// @brief 设置亮度值
    void setBrightness(GLfloat brightness) { mainSettings.brightness = brightness; }

    /// @return 当前饱和度值
    [[nodiscard]] GLfloat getSaturation() const { return mainSettings.saturation; }

    /// @brief 设置饱和度值
    void setSaturation(GLfloat saturation) { mainSettings.saturation = saturation; }

    /// @return 当前对比度值
    [[nodiscard]] GLfloat getContrast() const { return mainSettings.contrast; }

    /// @brief 设置对比度值
    void setContrast(GLfloat contrast) { mainSettings.contrast = contrast; }


private:
    QOpenGLShaderProgram *program = nullptr; // 着色器程序, 延迟初始化 (首次 beforeRendering 时创建)
    QMatrix4x4 viewMatrix;                   // 正交投影矩阵, 用于裁掉 FFmpeg 行对齐产生的无效边

    // 两套设置副本: mainSettings 仅 GUI 线程读写, renderSettings 仅渲染线程读写
    PONY_GUARD_BY(MAIN)   RenderSettings mainSettings;
    PONY_GUARD_BY(RENDER) RenderSettings renderSettings;

    /**
     * @brief 生成一张 2D 纹理并设置采样参数
     * @param texture 输出纹理句柄
     * @note 采样参数: REPEAT 环绕 + 双线性过滤; 调用前需先 glActiveTexture 选中纹理单元
     */
    void inline createTextureBuffer(GLuint *texture) {
        QOpenGLFunctions_3_3_Core::glGenTextures(1, texture);
        QOpenGLFunctions_3_3_Core::glBindTexture(GL_TEXTURE_2D, *texture);
        QOpenGLFunctions_3_3_Core::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        QOpenGLFunctions_3_3_Core::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        QOpenGLFunctions_3_3_Core::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        QOpenGLFunctions_3_3_Core::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

private:
    // OpenGL 对象句柄
    GLuint vao = 0, vbo = 0, ebo = 0;                       // 顶点数组对象 / 顶点缓冲 / 索引缓冲
    GLuint textureY = 0, textureU = 0, textureV = 0, textureLUT = 0; // Y/U/V 平面纹理 + LUT 滤镜纹理
    GLint brightnessLoc = -1, contrastLoc = -1,  saturationLoc = -1; // 着色器 uniform 位置 (init 时缓存)
public  slots:
    /**
     * @brief 初始化 OpenGL 资源 (渲染线程, 每次 beforeRendering 触发, 幂等)
     *
     * 流程:
     *   1. 仅首次打印一次 OpenGL / GLSL 版本与厂商信息
     *   2. 编译并链接 vertex.vsh / fragment.fsh, 缓存调节项的 uniform 位置
     *   3. 创建 VAO/VBO/EBO 并上传全屏矩形的顶点与索引数据
     *   4. 在纹理单元 0~3 上创建 Y/U/V/LUT 四张纹理
     */
    void init() {
        if (program) { return; } // 已初始化, 直接返回
        program = new QOpenGLShaderProgram;
        // 获取函数指针（初始化 GL 函数表）
        QOpenGLFunctions_3_3_Core::initializeOpenGLFunctions();
        // 打印一次 OpenGL 信息
        static bool info = false;
        if (!info) {
            qInfo() << "OpenGL version:"
                    << QLatin1String(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
            qInfo() << "GLSL version:"
                    << QLatin1String(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
            qInfo() << "Vendor:" << QLatin1String(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
            qInfo() << "Renderer:" << QLatin1String(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
            info = true;
        }
        // 加载着色器
        program->addCacheableShaderFromSourceFile(QOpenGLShader::Vertex, u":/player/shader/vertex.vsh"_qs);
        program->addCacheableShaderFromSourceFile(QOpenGLShader::Fragment, u":/player/shader/fragment.fsh"_qs);
        // 链接为可执行程序
        program->link();
        // 设为当前程序
        program->bind();
        // 查询着色器里 brightness/contrast/saturation 三个 uniform 的位置并缓存到成员
        brightnessLoc = program->uniformLocation("brightness");
        contrastLoc = program->uniformLocation("contrast");
        saturationLoc = program->uniformLocation("saturation");
        // 本项目由场景图管理清屏，不需要
//        glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
        // 一个VAO（顶点数组对象）打包顶点布局
        glGenVertexArrays(1, &vao);
        // 一个VBO（顶点缓冲对象）存顶点
        glGenBuffers(1, &vbo);
        // 一个EBO（索引缓冲对象）存索引
        glGenBuffers(1, &ebo);

        // 把VBO绑定到GL_ARRAY_BUFFER 目标
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        // 分配显存并把顶点数据拷进去
        glBufferData(GL_ARRAY_BUFFER, sizeof(VERTEX_POS), VERTEX_POS, GL_STATIC_DRAW);

        // 绑定 vao,接下来设置的属性格式会被"记录"进这个 VAO
        glBindVertexArray(vao);
        // 描述属性0的格式
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), ZERO_OFFSET); // 位置属性 (前 3 个分量)
        // 启用属性0
        glEnableVertexAttribArray(0);
        // 描述属性1的格式
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), // 纹理坐标属性 (后 2 个分量)
                              reinterpret_cast<const void *>(3 * sizeof(GLfloat)));
        // 启用属性1
        glEnableVertexAttribArray(1);
        // 把EBO绑定到GL_ELEMENT_ARRAY_BUFFER（同样记录在VAO中）
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        // 把索引数据（6 个 GLuint）上传进 ebo
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(VERTEX_INDEX), VERTEX_INDEX, GL_STATIC_DRAW);

        // 把四个采样器绑定到固定的纹理单元: 0=Y, 1=U, 2=V, 3=LUT
        program->setUniformValue("tex_y", 0);
        program->setUniformValue("tex_u", 1);
        program->setUniformValue("tex_v", 2);
        program->setUniformValue("tex_lut", 3);
        // 启用2D纹理功能
        glEnable(GL_TEXTURE_2D);
        // 激活纹理单元0
        glActiveTexture(GL_TEXTURE0);
        // 在单元0生成Y平面纹理
        createTextureBuffer(&textureY);
        glActiveTexture(GL_TEXTURE1);
        createTextureBuffer(&textureU);
        glActiveTexture(GL_TEXTURE2);
        createTextureBuffer(&textureV);
        glActiveTexture(GL_TEXTURE3);
        createTextureBuffer(&textureLUT);
        // 最后把 LUT 纹理重新绑定到当前单元（3）
        glBindTexture(GL_TEXTURE_2D, textureLUT);
    };

    /**
     * @brief 将 GUI 线程的设置同步到渲染线程 (渲染线程, 每次 beforeSynchronizing 触发)
     * @note 此时 GUI 线程与渲染线程都被阻塞, 可安全读写 mainSettings
     */
    void sync() {
        renderSettings.updateBy(mainSettings);
    }
public:
    // 仅允许在 GUI 线程构造
    PONY_GUARD_BY(MAIN) FireworksRenderer() {
        qDebug() << "Create Hurricane Renderer:" << static_cast<void *>(this) << ".";
    }

    /**
     * @brief 设置待渲染的视频帧 (仅 GUI 线程)
     * @param frame 新视频帧的引用 (引用计数共享, 零拷贝)
     * @return 帧是否有变化; 返回 false 表示与当前帧相同, 上层无需触发重绘
     */
    PONY_GUARD_BY(MAIN) bool setVideoFrame(const VideoFrameRef &frame) {
        if (static_cast<const VideoFrameRef&>(mainSettings.videoFrame) == frame) {
            return false;
        } {
            mainSettings.videoFrame = frame;
            return true;
        }
    }

    /// @brief 设置 LUT 滤镜图像 (仅 GUI 线程; 空图像表示清除滤镜)
    PONY_GUARD_BY(MAIN) void setLUTFilter(const QImage& image) {
        mainSettings.lutFilter = image;
    }


public:
    /**
     * @brief 声明节点只在自身边界矩形内渲染 (供场景图裁剪优化)
     */
    [[nodiscard]] RenderingFlags flags() const override {
        return BoundedRectRendering;
    }

    /**
     * @brief 声明渲染会改变混合 / 裁剪 / 模板状态,
     *        场景图不会为这些渲染状态做任何假设
     */
    [[nodiscard]] StateFlags changedStates() const override {
        return BlendState | ScissorState | StencilState;
    }

    /**
     * @brief 渲染一帧视频 (渲染线程)
     * @param state 场景图提供的渲染状态, 包含裁剪矩形等信息
     *
     * 流程:
     *   1. 启用混合 (alpha 混合) 与裁剪测试, 防止画面溢出到组件边界之外;
     *   2. 按 keepFrameRate 计算等比缩放的视口 (letterbox, 上下或左右留黑边);
     *   3. 帧尺寸变化时用 glTexImage2D 整体重传纹理,
     *      仅内容变化时用 glTexSubImage2D 局部更新 (Y/U/V/LUT 四张);
     *   4. glDrawElements 绘制全屏矩形。
     */
    void render(const RenderState *state) override  {
        // 在渲染线程调用

        // 由于 QTBUG-97589, 无法从场景图获得模型-视图矩阵
        // https://bugreports.qt.io/browse/QTBUG-97589
        // 变通方案: 假设父级裁剪区域即为 hurricane 的裁剪区域
        const QRect r = state->scissorRect();

        // 开启alpha混合
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        if (state->scissorEnabled()) {
            // 开启裁剪测试
            glEnable(GL_SCISSOR_TEST);
            // 只允许在组件矩形内绘制
            glScissor(r.x(), r.y(), r.width(), r.height());
        } else {
            ILLEGAL_STATE("Scissor Test must be enabled. For example: wrap Fireworks "
                                     "in a Rectangle and set clip: true. ");
        }
        if (state->stencilEnabled()) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, state->stencilValue(), 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }

        // 绑定程序
        program->bind();

        // getUpdate(): 读取最新值并清除脏标记
        // 将三个值上传给片元着色器
        program->setUniformValue(saturationLoc, renderSettings.saturation.getUpdate());
        program->setUniformValue(contrastLoc, renderSettings.contrast.getUpdate());
        program->setUniformValue(brightnessLoc, renderSettings.brightness.getUpdate());
        // 绑定顶点数组对象
        glBindVertexArray(vao);
        // 绑定顶点缓冲对象
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        // 绑定索引缓冲对象
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        // 获取要渲染的帧图像
        const VideoFrameRef &videoFrame = renderSettings.videoFrame;
        if (!videoFrame.isValid()) { return; }
        int lineSize = videoFrame.getLineSize(); // 行跨度 (含对齐填充)
        int imageHeight = videoFrame.getHeight();
        int imageWidth = videoFrame.getWidth();
        auto *imageY = videoFrame.getY();
        auto *imageU = videoFrame.getU();
        auto *imageV = videoFrame.getV();
        if (renderSettings.keepFrameRate) {
            // 保持原始宽高比: 计算适配裁剪区域的视口, 不足方向居中留黑边
            // 帧的宽高比
            double rate = static_cast<double>(imageHeight) / static_cast<double>(imageWidth);
            // // 按 r 宽度等比算出的高度
            double h = r.width() * rate;
            if (h >= r.height()) {
                // 高度受限: 左右留黑边
                // 适配后的宽度
                double wFit = r.height() / rate;
                // // 左右总黑边宽
                double wPad = r.width() - wFit;
                //  // 水平居中
                glViewport(r.x() + static_cast<GLsizei>(wPad) / 2, r.y(), static_cast<GLsizei>(wFit), r.height());
            } else {
                // 宽度受限: 上下留黑边
                double hFit = r.width() * rate;
                double hPad =  r.height() - hFit;
                glViewport(r.x(), r.y()  + static_cast<GLsizei>(hPad) / 2, r.width(), static_cast<GLsizei>(hFit));
            }
        } else {
            // 不保持宽高比: 直接拉伸铺满
            glViewport(r.x(), r.y(), r.width(), r.height());
        }

        // 只读当前值不清除脏标记
        QImage lutTexture = renderSettings.lutFilter;

        if (renderSettings.videoFrame.isUpdateSize()) {
            // 帧尺寸变化: 重新分配全部四张纹理
            // 由于 FFmpeg 可能对帧做行对齐填充, 需用正交投影裁掉无效数据
            viewMatrix.setToIdentity();
            // 构造正交投影
            viewMatrix.ortho(0, static_cast<float>(imageWidth) / static_cast<float>(lineSize), 0, 1, -1, 1);
//            viewMatrix.translate(0, 0, -0.7F);
            // 上传矩阵给view
            program->setUniformValue("view", viewMatrix);
            // 上传yuv平面到纹理单元
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureY);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, lineSize, imageHeight, 0, GL_RED, GL_UNSIGNED_BYTE, imageY);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textureU);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, lineSize / 2, imageHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, imageU);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, textureV);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, lineSize / 2, imageHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, imageV);
            glActiveTexture(GL_TEXTURE3);
            // 上传LUT滤镜图到纹理单元
            glBindTexture(GL_TEXTURE_2D, textureLUT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, lutTexture.width(), lutTexture.height(), 0, GL_RGB, GL_UNSIGNED_BYTE, lutTexture.constBits());
        } else if (renderSettings.videoFrame.isUpdate()) {
            // 仅内容变化 (尺寸不变): 局部更新纹理区域, 避免整张重传
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureY);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lineSize, imageHeight, GL_RED, GL_UNSIGNED_BYTE, imageY);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textureU);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lineSize / 2, imageHeight / 2, GL_RED, GL_UNSIGNED_BYTE, imageU);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, textureV);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lineSize / 2, imageHeight / 2, GL_RED, GL_UNSIGNED_BYTE, imageV);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, textureLUT);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lutTexture.width(), lutTexture.height(), GL_RGB, GL_UNSIGNED_BYTE, lutTexture.constBits());
        }
        // 开始绘制
        glDrawElements(GL_TRIANGLES, sizeof(VERTEX_INDEX) / sizeof(GLuint), GL_UNSIGNED_INT, ZERO_OFFSET);
        // 解绑着色器程序
        program->release();
    }

    /// @brief 析构 — 解绑并释放 OpenGL 资源 (渲染线程)
    ~FireworksRenderer() override {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        program->release();
        qDebug() << "Deconstruct Hurricane Renderer:" << static_cast<void *>(this) << ".";
    }


};
