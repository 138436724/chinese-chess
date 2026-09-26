// ============================================================================
// image_stats.cpp — 图像统计小工具(离线验证用,不参与运行时)
//
// 用途:给渲染截图(EXR/PNG)输出可复现的数值读数,供
//   · 判断渲染是否正常(不是全黑/NaN/过曝)
//   · 阶段间回归对比(tools/compare_exr.py 的数值来源)
//
// 为什么需要独立工具:系统 Python 没有 OpenImageIO 绑定,而项目在 C++ 侧
// 已经链接 OIIO(vcpkg)。比起在 Python 里手写 EXR 解码器,直接复用 OIIO 更可靠。
//
// 构建(见 tools/build_image_stats.ps1):
//   cl /std:c++20 /EHsc /O2 /I <vcpkg>/include image_stats.cpp /link <vcpkg>/lib/OpenImageIO.lib
// ============================================================================

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr float kOverExposed = 1.0f;
constexpr float kNearBlack   = 1e-4f;

float luminance(float r, float g, float b) noexcept
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float percentile(const std::vector<float>& _sorted, double _q) noexcept
{
    if (_sorted.empty())
    {
        return 0.0f;
    }
    const auto idx = static_cast<size_t>(_q * static_cast<double>(_sorted.size() - 1) + 0.5);
    return _sorted[std::min(idx, _sorted.size() - 1)];
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: image_stats <image> [--blocks R C] [--dots x1,y1,x2,y2,...] [--dot-radius N]\n");
        std::printf("  --blocks R C     R x C 分块亮度矩阵\n");
        std::printf("  --dots  x,y,...  在给定像素点附近取圆盘均值(白炉球体阵逐球读数)\n");
        std::printf("  --dot-radius N   圆盘半径(像素,默认 12)\n");
        return 2;
    }

    const std::string path = argv[1];

    int blocks_r = 0;
    int blocks_c = 0;
    int dot_radius = 12;
    std::vector<std::pair<int, int>> dots;

    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--blocks" && i + 2 < argc)
        {
            blocks_r = std::atoi(argv[i + 1]);
            blocks_c = std::atoi(argv[i + 2]);
            i += 2;
        }
        else if (a == "--dot-radius" && i + 1 < argc)
        {
            dot_radius = std::atoi(argv[i + 1]);
            ++i;
        }
        else if (a == "--dots" && i + 1 < argc)
        {
            // 逗号分隔的 x,y 对
            const char* s = argv[i + 1];
            for (;;)
            {
                int x = 0;
                int y = 0;
                int consumed = 0;
                if (std::sscanf(s, "%d,%d%n", &x, &y, &consumed) != 2)
                {
                    break;
                }
                dots.emplace_back(x, y);
                s += consumed;
                if (*s == ',')
                {
                    ++s;
                }
                else
                {
                    break;
                }
            }
            ++i;
        }
    }

    auto image = OIIO::ImageInput::open(path);
    if (!image)
    {
        std::printf("ERROR: 无法打开 %s: %s\n", path.c_str(), OIIO::geterror().c_str());
        return 1;
    }

    const OIIO::ImageSpec& spec = image->spec();
    const size_t           count =
        static_cast<size_t>(spec.width) * static_cast<size_t>(spec.height) * static_cast<size_t>(spec.nchannels);

    std::vector<float> pixels(count, 0.0f);
    if (!image->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data(), sizeof(float) * spec.nchannels,
                           sizeof(float) * spec.nchannels * spec.width, OIIO::AutoStride))
    {
        std::printf("ERROR: 读取像素失败: %s\n", OIIO::geterror().c_str());
        return 1;
    }
    image->close();

    std::printf("FILE      %s\n", path.c_str());
    // 注意:ImageSpec::format 是 TypeDesc 数据成员(不是函数),c_str() 才是方法
    std::printf("SIZE      %d x %d, %d channels, format=%s\n", spec.width, spec.height, spec.nchannels,
                spec.format.c_str());

    // ---- 逐通道统计 ----
    size_t nonfinite = 0;
    for (int c = 0; c < spec.nchannels; ++c)
    {
        double     sum = 0.0;
        float      mn = 1e30f;
        float      mx = -1e30f;
        size_t     bad = 0;
        for (size_t i = 0; i < static_cast<size_t>(spec.width) * spec.height; ++i)
        {
            const float v = pixels[i * spec.nchannels + c];
            if (!std::isfinite(v))
            {
                ++bad;
                continue;
            }
            sum += static_cast<double>(v);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        nonfinite += bad;
        const double n = static_cast<double>(static_cast<size_t>(spec.width) * spec.height - bad);
        std::printf("CHANNEL   c%d  mean=%.6f  min=%.6f  max=%.6f  nonfinite=%zu\n", c,
                    n > 0.0 ? sum / n : 0.0, mn, mx, bad);
    }

    // ---- 亮度统计(BT.709)----
    std::vector<float> lum;
    lum.reserve(static_cast<size_t>(spec.width) * spec.height);
    for (size_t i = 0; i < static_cast<size_t>(spec.width) * spec.height; ++i)
    {
        const float* p = &pixels[i * spec.nchannels];
        const float  r = p[0];
        const float  g = spec.nchannels > 1 ? p[1] : r;
        const float  b = spec.nchannels > 2 ? p[2] : r;
        const float  v = luminance(r, g, b);
        if (std::isfinite(v))
        {
            lum.push_back(v);
        }
    }
    std::sort(lum.begin(), lum.end());

    double sum = 0.0;
    size_t over = 0;
    size_t dark = 0;
    for (float v : lum)
    {
        sum += static_cast<double>(v);
        over += (v > kOverExposed) ? 1 : 0;
        dark += (v < kNearBlack) ? 1 : 0;
    }
    const double n = static_cast<double>(lum.size());

    std::printf("LUMINANCE mean=%.6f  p01=%.6f  p05=%.6f  p25=%.6f  p50=%.6f  p75=%.6f  p95=%.6f  p99=%.6f  max=%.6f\n",
                n > 0.0 ? sum / n : 0.0, percentile(lum, 0.01), percentile(lum, 0.05), percentile(lum, 0.25),
                percentile(lum, 0.50), percentile(lum, 0.75), percentile(lum, 0.95), percentile(lum, 0.99),
                lum.empty() ? 0.0f : lum.back());
    std::printf("FRACTION  over(>1)=%.4f%%  near_black(<1e-4)=%.4f%%  nonfinite_pixels=%zu\n",
                100.0 * static_cast<double>(over) / std::max(n, 1.0), 100.0 * static_cast<double>(dark) / std::max(n, 1.0),
                nonfinite);

    // ---- 分块亮度矩阵 ----
    // 白炉球体阵的逐格核查需要"哪一格异常"而不是全局均值:
    // 球体按 (roughness 列 × metallic 行) 排布,在分块矩阵里会呈现规则的点阵,
    // 于是可以逐列/逐行比较亮度,定位是哪个参数组合出问题。
    if (blocks_r > 0 && blocks_c > 0 && spec.width > 0 && spec.height > 0)
    {
        std::printf("BLOCKS    %d x %d(每格为该区域的平均亮度)\n", blocks_r, blocks_c);
        for (int br = 0; br < blocks_r; ++br)
        {
            std::printf("  ");
            for (int bc = 0; bc < blocks_c; ++bc)
            {
                const int y0 = br * spec.height / blocks_r;
                const int y1 = std::max(y0 + 1, (br + 1) * spec.height / blocks_r);
                const int x0 = bc * spec.width / blocks_c;
                const int x1 = std::max(x0 + 1, (bc + 1) * spec.width / blocks_c);

                double sum_b = 0.0;
                size_t cnt   = 0;
                for (int y = y0; y < y1; ++y)
                {
                    for (int x = x0; x < x1; ++x)
                    {
                        const float* p = &pixels[(static_cast<size_t>(y) * spec.width + x) * spec.nchannels];
                        const float  g = spec.nchannels > 1 ? p[1] : p[0];
                        const float  b = spec.nchannels > 2 ? p[2] : p[0];
                        const float  v = luminance(p[0], g, b);
                        if (std::isfinite(v))
                        {
                            sum_b += static_cast<double>(v);
                            ++cnt;
                        }
                    }
                }
                std::printf("%7.4f ", cnt > 0 ? sum_b / static_cast<double>(cnt) : 0.0);
            }
            std::printf("\n");
        }
    }

    // ---- 指定点位采样(白炉球体阵逐球读数)----
    // 球心在屏幕上的位置可由"世界坐标 → NDC → 像素"算出(见 tools/furnace_probe.ps1),
    // 这里只负责在给定点取圆盘均值。逐球读数才能看出"哪一格异常"。
    if (!dots.empty())
    {
        std::printf("DOTS      radius=%d px, %zu 点(x y lum r g b)\n", dot_radius, dots.size());
        for (const auto& [cx, cy] : dots)
        {
            double sum = 0.0;
            double sr = 0.0;
            double sg = 0.0;
            double sb = 0.0;
            size_t cnt = 0;
            for (int dy = -dot_radius; dy <= dot_radius; ++dy)
            {
                for (int dx = -dot_radius; dx <= dot_radius; ++dx)
                {
                    if (dx * dx + dy * dy > dot_radius * dot_radius)
                    {
                        continue;
                    }
                    const int x = cx + dx;
                    const int y = cy + dy;
                    if (x < 0 || y < 0 || x >= spec.width || y >= spec.height)
                    {
                        continue;
                    }
                    const float* p = &pixels[(static_cast<size_t>(y) * spec.width + x) * spec.nchannels];
                    const float  g = spec.nchannels > 1 ? p[1] : p[0];
                    const float  b = spec.nchannels > 2 ? p[2] : p[0];
                    const float  v = luminance(p[0], g, b);
                    if (!std::isfinite(v))
                    {
                        continue;
                    }
                    sum += static_cast<double>(v);
                    sr += static_cast<double>(p[0]);
                    sg += static_cast<double>(g);
                    sb += static_cast<double>(b);
                    ++cnt;
                }
            }
            const double inv = cnt > 0 ? 1.0 / static_cast<double>(cnt) : 0.0;
            std::printf("  %4d %4d  %.6f  %.6f %.6f %.6f  (n=%zu)\n", cx, cy, sum * inv, sr * inv, sg * inv, sb * inv, cnt);
        }
    }
    return 0;
}
