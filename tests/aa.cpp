// aa/AntiAliasing.hpp (and AntiAliasingD3D11.hpp, which uses the same parts): the API-independent parts, matrices, the
// Halton jitter, JitterProjection, the camera's near/far/fov and the mode fallback without the hub. Rendering needs a
// device and a game: the standalone header check compiles the rest.

#include "../aa/AntiAliasingD3D11.hpp"
#include "check.h"
#include <cmath>

namespace
{
    bool Near(double a, double b, double epsilon = 1e-5)
    {
        return std::abs(a - b) <= epsilon;
    }

    // A left-handed perspective projection with standard [0, 1] depth, as D3DXMatrixPerspectiveFovLH builds it
    WFPAA::Matrix Perspective(float fovY, float aspect, float zn, float zf)
    {
        WFPAA::Matrix p;
        double yScale = 1.0 / std::tan(fovY / 2.0);
        p.m[0][0] = yScale / aspect;
        p.m[1][1] = yScale;
        p.m[2][2] = zf / (zf - zn);
        p.m[2][3] = 1.0;
        p.m[3][2] = -zn * zf / (zf - zn);
        return p;
    }
}

TEST_CASE("aa: matrix product and inverse")
{
    float f[16] = { 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1 };
    auto m = WFPAA::Matrix::From(f);
    auto i = m.Inverse();
    auto id = m * i;
    bool identity = true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            identity = identity && Near(id.m[r][c], r == c ? 1.0 : 0.0);
    CHECK(identity);
    float back[16];
    m.To(back);
    CHECK(std::equal(std::begin(f), std::end(f), std::begin(back)));

    // A singular matrix gives the identity instead of garbage
    WFPAA::Matrix zero;
    auto z = zero.Inverse();
    CHECK(z.m[0][0] == 1.0 && z.m[3][3] == 1.0 && z.m[0][1] == 0.0);
}

TEST_CASE("aa: Halton sequence")
{
    CHECK(WFPAA::Halton(0, 2) == 0.0f);
    CHECK(WFPAA::Halton(1, 2) == 0.5f);
    CHECK(WFPAA::Halton(2, 2) == 0.25f);
    CHECK(WFPAA::Halton(3, 2) == 0.75f);
    CHECK(Near(WFPAA::Halton(1, 3), 1.0 / 3.0));
    CHECK(Near(WFPAA::Halton(2, 3), 2.0 / 3.0));
    CHECK(Near(WFPAA::Halton(4, 3), 4.0 / 9.0));
    // In [0, 1) and well spread: every quarter of the first 16 points of base 2 has four
    int quarters[4] = {};
    for (uint32_t k = 1; k <= 16; ++k)
    {
        auto h = WFPAA::Halton(k, 2);
        CHECK(h >= 0.0f && h < 1.0f);
        ++quarters[(std::min)(3, static_cast<int>(h * 4.0f))];
    }
    CHECK(quarters[0] == 4 && quarters[1] == 4 && quarters[2] == 4 && quarters[3] == 4);
}

TEST_CASE("aa: JitterProjection moves the image by the jitter in pixels, x right, y down")
{
    auto p = Perspective(1.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    float P[16];
    p.To(P);
    WFPAA::Jitter jitter{ 0.25f, -0.5f };
    float width = 1920.0f, height = 1080.0f;
    WFPAA::JitterProjection(P, jitter, width, height);

    // A point in view space, its NDC before and after
    double v[4] = { 3.0, -2.0, 50.0, 1.0 };
    auto project = [&](const float* M, double out[2])
    {
        double clip[4] = {};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                clip[c] += v[r] * M[r * 4 + c];
        out[0] = clip[0] / clip[3];
        out[1] = clip[1] / clip[3];
    };
    float original[16];
    p.To(original);
    double a[2], b[2];
    project(original, a);
    project(P, b);
    // NDC x grows to the right, y up; pixels: x right, y down
    CHECK(Near((b[0] - a[0]) * width / 2.0, jitter.X, 1e-3));
    CHECK(Near(-(b[1] - a[1]) * height / 2.0, jitter.Y, 1e-3));
}

TEST_CASE("aa: without the hub DLAA and FSR fall back to TAA, no jitter before anything is resolved")
{
    using WFPAA::TemporalAA;
    CHECK(!TemporalAA::IsAvailable(WFPAA::Backend::DLSS));
    CHECK(!TemporalAA::IsAvailable(WFPAA::Backend::FSR));
    TemporalAA::SetMode(WFPAA::Mode::DLAA, 0.5f);
    CHECK(TemporalAA::EffectiveMode() == WFPAA::Mode::TAA);
    TemporalAA::SetMode(WFPAA::Mode::FSR, 2.0f);
    CHECK(TemporalAA::EffectiveMode() == WFPAA::Mode::TAA);
    TemporalAA::SetMode(WFPAA::Mode::Off, 0.0f);
    CHECK(TemporalAA::EffectiveMode() == WFPAA::Mode::Off);

    TemporalAA::SetMode(WFPAA::Mode::TAA, 0.0f);
    CHECK(!TemporalAA::IsJitterActive());
    WFPAA::Camera camera;
    camera.View = WFPAA::Matrix::Identity();
    camera.Projection = Perspective(1.0f, 4.0f / 3.0f, 0.5f, 500.0f);
    camera.Width = 1280;
    camera.Height = 960;
    auto jitter = TemporalAA::OnCamera(camera);
    CHECK(jitter.X == 0.0f && jitter.Y == 0.0f);
    TemporalAA::SetMode(WFPAA::Mode::Off, 0.0f);
}

TEST_CASE("aa: the shared protocol's backends")
{
    CHECK(static_cast<uint32_t>(WFPAA::Backend::DLSS) == static_cast<uint32_t>(UpscalerBridge::Backend::DLSS));
    CHECK(static_cast<uint32_t>(WFPAA::Backend::FSR) == static_cast<uint32_t>(UpscalerBridge::Backend::FSR));
}

TEST_CASE("aa: JitterOf follows the mode's phases, one Halton step per game frame")
{
    using WFPAA::TemporalAA;
    auto options = WFPAA::Options();
    options.JitterPhases = 8;
    options.FSRJitterPhases = 8;
    TemporalAA::Configure(options);
    TemporalAA::SetMode(WFPAA::Mode::TAA, 0.0f);

    // In (-0.5, 0.5), the Halton(2, 3) point of (frame % phases) + 1
    for (uint32_t frame = 0; frame < 20; ++frame)
    {
        auto jitter = TemporalAA::JitterOf(frame);
        CHECK(jitter.X > -0.5f && jitter.X < 0.5f && jitter.Y > -0.5f && jitter.Y < 0.5f);
        CHECK(jitter.X == WFPAA::Halton(frame % 8 + 1, 2) - 0.5f);
        CHECK(jitter.Y == WFPAA::Halton(frame % 8 + 1, 3) - 0.5f);
    }
    // The cycle repeats after the phases
    CHECK(TemporalAA::JitterOf(3).X == TemporalAA::JitterOf(11).X);

    // Consecutive frames alternate between the halves of the pixel horizontally (two steps per frame would not)
    int left = 0;
    for (uint32_t frame = 0; frame < 8; ++frame)
        left += TemporalAA::JitterOf(frame).X < 0.0f ? 1 : 0;
    CHECK(left >= 3 && left <= 5);

    TemporalAA::SetMode(WFPAA::Mode::Off, 0.0f);
    TemporalAA::Configure(WFPAA::Options());
}

TEST_CASE("aa: the staged API does nothing with anti-aliasing off and before a frame")
{
    using WFPAA::TemporalAA;
    TemporalAA::SetMode(WFPAA::Mode::Off, 0.0f);
    CHECK(!TemporalAA::RenderMotion(nullptr, nullptr));
    CHECK(TemporalAA::MotionTexture() == nullptr);
    CHECK(TemporalAA::StandardDepthTexture() == nullptr);
    CHECK(!TemporalAA::Resolve(nullptr, nullptr, nullptr));
    CHECK(!TemporalAA::IsSceneResolved());

    // A frame begun without a device: the camera is taken, nothing is drawn
    TemporalAA::SetMode(WFPAA::Mode::TAA, 0.0f);
    TemporalAA::BeginFrame(nullptr, WFPAA::Matrix::Identity(), Perspective(1.0f, 16.0f / 9.0f, 0.25f, 4500.0f), { 0.25f, -0.125f }, 1920, 1080);
    CHECK(!TemporalAA::RenderMotion(nullptr, nullptr));
    CHECK(!TemporalAA::IsSceneResolved());
    TemporalAA::SetMode(WFPAA::Mode::Off, 0.0f);
}
