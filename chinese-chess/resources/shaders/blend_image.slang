// ============================================================================
// blend_image.slang — 场景 + UI 合成 Pass（最终输出到交换链）
// 全屏三角形（顶点由索引生成，零顶点缓冲）采样场景纹理与 UI 纹理，
// 按 UI alpha 混合；场景侧先过 OCIO 色彩转换（ACES 2.0 显示变换）。
// OCIO 色彩转换：编译期由 ocio_helper::replace_and_compile 将
// `ocio_conversion` 桩函数替换为 OCIO 生成的着色器代码（USE_OCIO 时）。
// 注意：桩函数必须保持单行且签名不变，替换逻辑依赖该行文本。
// ============================================================================

// 顶点 → 片元插值输出
struct vs_output
{
  float4 position : SV_Position;
  float2 uv;   // 屏幕 UV（[0,1]）
};

Texture2D    image_scene;  // 场景渲染输出（R16G16B16A16_SFLOAT）
Texture2D    image_ui;     // UI 渲染输出（MSAA resolve 后）
SamplerState image_sampler;

[shader("vertex")]
vs_output vertMain(uint vertex_id: SV_VertexID)
{
  // 全屏三角形：用顶点索引生成角点 UV（(0,0)/(2,0)/(0,2) 模式），
  // 再映射到裁剪坐标 [-1,1]×[-1,1]——不读任何顶点缓冲
  vs_output output;
  output.uv       = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
  return output;
}

// OCIO 色彩转换占位：USE_OCIO 时编译期被替换为 ocio_helper 生成的 GPU 变换代码
float4 ocio_conversion(float4 in_pixel) { return in_pixel; }

[shader("fragment")]
float4 fragMain(vs_output input)
{
  // 场景 → OCIO 转换（ACES 2.0）；UI 直接采样（UI 已在 sRGB 空间）
  float4 color_scene = ocio_conversion(image_scene.Sample(image_sampler, input.uv));
  float4 color_ui    = image_ui.Sample(image_sampler, input.uv);
  // 按 UI 的 alpha 混合：lerp(场景, UI, UI.a)
  float3 color_final = lerp(color_scene.rgb, color_ui.rgb, color_ui.a);
  return float4(color_final, 1.f);
}
