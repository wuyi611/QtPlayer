/**
 * @file fragment.fsh
 * @brief 片元着色器 — 完成视频帧的最终上屏着色
 *
 * 输入:
 *   - 纹理: tex_y / tex_u / tex_v  (YUV420P 三个平面, 由 render() 每帧上传)
 *   - 纹理: tex_lut               (2D LUT 颜色查找表, 换滤镜时上传)
 *   - uniform: brightness / contrast / saturation (由 render() 每帧设置)
 *
 * 处理流水线:
 *   1. 从 Y/U/V 三张纹理采样, 还原 YUV 颜色
 *   2. YUV → RGB 转换 (BT.601 矩阵)
 *   3. 依次施加 饱和度 → 对比度 → 亮度 调整 (三个 4×4 矩阵相乘)
 *   4. 最后套 LUT 滤镜 (颜色查找表, 可选)
 *
 * 注意: GLSL 中 mat4/mat3 构造函数是【列主序】填充的,
 *       每行参数对应一【列】, 阅读矩阵时需按"列"理解。
 */
#version 330 core

// ---- 输入纹理 ----
uniform sampler2D tex_y;   // Y 平面纹理 (单通道, 亮度; 尺寸 W×H)
uniform sampler2D tex_u;   // U 平面纹理 (单通道, 色度; 尺寸 W/2×H/2)
uniform sampler2D tex_v;   // V 平面纹理 (单通道, 色度; 尺寸 W/2×H/2)
uniform sampler2D tex_lut; // LUT 滤镜纹理 (RGB; 512×512 颜色查找表, 1×1 表示无滤镜)

// ---- 颜色调节参数 (由 render() 用 getUpdate() 传入) ----
uniform float brightness;  // 亮度调整 (0.0 ~ 2.0, 默认 1.0 附近; 0 偏黑, 2 偏亮)
uniform float contrast;    // 对比度调整 (0.0 ~ 2.0, 默认 1.0)
uniform float saturation;  // 饱和度调整 (0.0 ~ 2.0, 默认 1.0)

in vec2 TexCoord;          // 顶点着色器传来的纹理坐标 (0~1)
out vec4 FragColor;        // 输出最终像素颜色


/**
 * @brief 2D LUT 颜色查找表滤镜
 * @param rgbColor 输入像素颜色 (RGB)
 * @return 滤镜处理后的颜色
 *
 * 原理: 把 512×512 的 LUT 图看作 8×8=64 个 64×64 的格子。
 *   - 蓝色通道 [0,1] → 0~63, 决定查【哪个格子】(B 通道的 64 个量化级);
 *   - 红/绿通道决定【格子内的位置】: R 对应 x 轴, G 对应 y 轴;
 *   - 查表得到的颜色即"滤镜处理后的颜色";
 *   - 蓝色落在两个格子之间时, 用 mix 对相邻两格结果线性插值, 保证平滑。
 *
 * 特殊约定: 若 LUT 纹理宽度为 1 (1×1 图), 视为"无滤镜", 原样返回输入色。
 */
vec4 lutFilter(vec4 rgbColor) {
    ivec2 lut_tex_size = textureSize(tex_lut, 0);  // 查询 LUT 纹理尺寸
    if (lut_tex_size.x == 1) { return rgbColor; }  // 1×1 = 无滤镜, 直接返回

    float blueColor = rgbColor.b * 63.;            // B 通道 → 0~63, 对应 64 个格子

    vec2 quad1;                                    // 当前格子 (向下取整)
    quad1.y = floor(floor(blueColor) / 8.0);       // 行号 = 蓝值 ÷ 8
    quad1.x = floor(blueColor) - (quad1.y * 8.0);  // 列号 = 蓝值 % 8

    vec2 quad2;                                    // 相邻格子 (向上取整, 供插值)
    quad2.y = floor(ceil(blueColor) / 8.0);
    quad2.x = ceil(blueColor) - (quad2.y * 8.0);

    vec2 texPos1;                                  // 格子内的采样坐标
    // 0.125    = 1/8: 单个格子的宽度比例 (512/8 = 64 像素)
    // 0.5/512  = 半像素偏移 (采样点对准像素中心)
    // (0.125 - 1.0/512.0) * r: 在格子内按 R 通道线性定位 x
    texPos1.x = (quad1.x * 0.125) + 0.5/512.0 + ((0.125 - 1.0/512.0) * rgbColor.r);
    // 同理按 G 通道定位 y
    texPos1.y = (quad1.y * 0.125) + 0.5/512.0 + ((0.125 - 1.0/512.0) * rgbColor.g);

    vec2 texPos2;                                  // 相邻格子的采样坐标 (同上)
    texPos2.x = (quad2.x * 0.125) + 0.5/512.0 + ((0.125 - 1.0/512.0) * rgbColor.r);
    texPos2.y = (quad2.y * 0.125) + 0.5/512.0 + ((0.125 - 1.0/512.0) * rgbColor.g);

    vec4 newColor1 = texture(tex_lut, texPos1);    // 采样当前格子 (低蓝值)
    vec4 newColor2 = texture(tex_lut, texPos2);    // 采样相邻格子 (高蓝值)

    return mix(newColor1, newColor2, fract(blueColor));  // 按蓝值小数部分线性插值
}


/**
 * @brief YUV → RGB 转换矩阵 (BT.601 标准, 列主序)
 *
 * 按列填充后, 行视角的矩阵为:
 *   [ 1        1         1       ]
 *   [ 0        -0.39465  2.03211 ]
 *   [ 1.13983  -0.58060  0       ]
 *
 * 对应公式:
 *   R = Y + 1.13983 × V
 *   G = Y - 0.39465 × U - 0.58060 × V
 *   B = Y + 2.03211 × U
 *
 * 注意: 采样时 U/V 已减去 0.5 (见 main), 因此这里无需再处理色度偏移。
 */
const mat3 YUV_RGB_TRANSFORM = mat3(
1,       1,        1,          // 第 1 列
0,       -0.39465, 2.03211,    // 第 2 列
1.13983, -0.58060, 0           // 第 3 列
);

/**
 * @brief 亮度调整矩阵 — 给 RGB 三个通道各加上 brightness
 *
 * 列主序填充后矩阵 (行视角):
 *   [ 1  0  0  b ]
 *   [ 0  1  0  b ]
 *   [ 0  0  1  b ]
 *   [ 0  0  0  1 ]
 * 效果: (r, g, b, 1) × M = (r+b, g+b, b+b, 1) — 整体提亮/压暗。
 *
 * 用 #define 把参数 b 代换成常量, 便于在矩阵构造中复用。
 */
mat4 brightnessMatrix(float brightness) {
    #define b brightness
    return mat4(
    1, 0, 0, 0,     // 第 1 列
    0, 1, 0, 0,     // 第 2 列
    0, 0, 1, 0,     // 第 3 列
    b, b, b, 1      // 第 4 列
    );
    #undef b
}

/**
 * @brief 对比度调整矩阵 — 以 0.5 为中心缩放颜色
 *
 * 列主序填充后矩阵 (行视角):
 *   [ c  0  0  m ]
 *   [ 0  c  0  m ]
 *   [ 0  0  c  m ]
 *   [ 0  0  0  1 ]
 * 其中 m = (1-c)/2, 效果: (c×v + m), 即颜色绕 0.5 中点缩放:
 *   - c > 1: 对比度增强 (向 0 和 1 两极拉伸);
 *   - c < 1: 对比度减弱 (向 0.5 灰收敛)。
 */
mat4 contrastMatrix(float contrast) {
    float minus = (1.0 - contrast) / 2.0;
    #define c contrast
    #define m minus
    return mat4(
    c, 0, 0, 0,     // 第 1 列
    0, c, 0, 0,     // 第 2 列
    0, 0, c, 0,     // 第 3 列
    m, m, m, 1      // 第 4 列
    );
    // 注: 原代码此处 #undef u / #undef v 是笔误, 应 #undef c / #undef m;
    // 宏名不同不影响编译, 这里保留原样。
    #undef u
    #undef v
}

/**
 * @brief 饱和度调整矩阵 — 按亮度权重混合"灰度"与"原色"
 *
 * 列主序填充后矩阵 (行视角):
 *   [ red.x   green.x   blue.x   0 ]
 *   [ red.y   green.y   blue.y   0 ]
 *   [ red.z   green.z   blue.z   0 ]
 *   [ 0       0         0        1 ]
 *
 * 原理: luminance = (0.3086, 0.6094, 0.0820) 是人眼亮度权重 (BT.601)。
 *   以红色行为例: red = 0.3086×(1-s) + s, 其余列保留 0.6094×(1-s)、0.0820×(1-s),
 *   即"原色比例 s + 灰度比例 (1-s)", 三行结构相同。
 *   - s = 1: 恒等变换, 原色输出;
 *   - s = 0: 纯灰度 (亮度加权);
 *   - s > 1: 饱和度增强。
 */
mat4 saturationMatrix(float saturation) {
    vec3 luminance = vec3(0.3086, 0.6094, 0.0820);  // 亮度权重 (BT.601)

    float minus = 1.0 - saturation;                 // 灰度混合比例

    vec3 red = vec3(luminance.x * minus);           // 第一行: 加权灰度
    red+= vec3(saturation, 0, 0);                   //         加上原色分量

    vec3 green = vec3(luminance.y * minus);         // 第二行
    green += vec3(0, saturation, 0);

    vec3 blue = vec3(luminance.z * minus);          // 第三行
    blue += vec3(0, 0, saturation);

    return mat4(
    red,     0,      // 第 1 列
    green,   0,      // 第 2 列
    blue,    0,      // 第 3 列
    0, 0, 0, 1       // 第 4 列
    );
}


/**
 * @brief 片元着色器主函数 — 每像素执行
 *
 * 流程:
 *   1. 从三张平面纹理采样 Y/U/V;
 *   2. YUV → RGB (BT.601 矩阵);
 *   3. 依次乘 饱和度 / 对比度 / 亮度 矩阵 (右乘顺序: 先饱和度, 再对比度, 最后亮度);
 *   4. 最后套 LUT 滤镜。
 */
void main(void) {
    vec3 yuv;
    vec3 rgb;
    yuv.x = texture(tex_y, TexCoord).r;            // Y: 亮度, 直接读 R 通道
    yuv.y = texture(tex_u, TexCoord).r - 0.5;      // U: 色度, 减 0.5 转为零中心 [-0.5, 0.5]
    yuv.z = texture(tex_v, TexCoord).r - 0.5;      // V: 色度, 同上
    rgb = YUV_RGB_TRANSFORM * yuv;                 // YUV → RGB (BT.601)
    // 先饱和度 → 对比度 → 亮度 (矩阵右乘, 最右的先作用)
    FragColor = brightnessMatrix(brightness) * contrastMatrix(contrast) * saturationMatrix(saturation) * vec4(rgb, 1);
    FragColor = lutFilter(FragColor);              // 最后套 LUT 滤镜 (可选)
}
