#include <RmlUi/Core.h>
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/CompiledFilterShader.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Backend.h>
#include <Shell.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#if defined RMLUI_PLATFORM_WIN32
    #include <RmlUi_Include_Windows.h>
#endif

#ifndef RMLUI_BGFX_EFFECTS_PROBE_DATA_DIR
    #define RMLUI_BGFX_EFFECTS_PROBE_DATA_DIR "samples/effects_probe/data"
#endif

namespace {

class ProbeFileInterface final : public Rml::FileInterface {
public:
    explicit ProbeFileInterface(std::string root) : root(std::move(root))
    {
        while (!this->root.empty() && (this->root.back() == '/' || this->root.back() == '\\')) {
            this->root.pop_back();
        }
    }

    Rml::FileHandle Open(const Rml::String& path) override
    {
        std::string normalized = path.c_str();
        while (!normalized.empty() && (normalized.front() == '/' || normalized.front() == '\\')) {
            normalized.erase(normalized.begin());
        }

        std::string full_path = root;
        full_path += '/';
        full_path += normalized;

        FILE* fp = std::fopen(full_path.c_str(), "rb");
        return reinterpret_cast<Rml::FileHandle>(fp);
    }

    void Close(Rml::FileHandle file) override
    {
        std::fclose(reinterpret_cast<FILE*>(file));
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
    {
        return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        return static_cast<size_t>(std::ftell(reinterpret_cast<FILE*>(file)));
    }

private:
    std::string root;
};

enum class ProbeKind {
    Document,
    DirectClipMask,
    CallbackTexture,
    RoundedShadowTexture,
    InsetBlurTexture,
    InsetNoBlurTexture,
    InsetChildCompositeTexture,
    InsetBlurNoClipTexture,
    InsetBlurInverseOnlyTexture,
};

struct ProbeCase {
    std::string_view key;
    std::string_view file;
    std::string_view description;
    ProbeKind kind = ProbeKind::Document;
};

constexpr ProbeCase kCases[] = {
    {"00", "00_baseline.rml", "plain rounded document, fixed header, scrolling"},
    {"01", "01_clip_body.rml", "body and child border-radius clipping"},
    {"02", "02_boxshadow_trail.rml", "box-shadow trail plus no-op filter layer"},
    {"03", "03_boxshadow_blur.rml", "blurred and inset box-shadow"},
    {"04", "04_filters.rml", "filter blur, drop-shadow, and color matrix"},
    {"05", "05_backdrop_filter.rml", "backdrop-filter over striped background"},
    {"06", "06_mask_image.rml", "mask-image with a gradient mask"},
    {"07", "07_scroll_stress.rml", "small scrollable set of mixed effect rows"},
    {"08", "", "direct RenderInterface SetInverse clip-mask probe", ProbeKind::DirectClipMask},
    {"09", "", "RenderManager callback texture SaveLayerAsTexture probe", ProbeKind::CallbackTexture},
    {"10", "", "RenderManager rounded box-shadow mesh callback texture probe", ProbeKind::RoundedShadowTexture},
    {"11", "11_boxshadow_outer_only.rml", "CSS box-shadow outer shadows only, no inset blur"},
    {"12", "12_boxshadow_inset.rml", "CSS inset box-shadow only and outer-plus-inset variants"},
    {"13", "", "manual RenderManager inset blur callback texture probe", ProbeKind::InsetBlurTexture},
    {"14", "", "manual RenderManager inset callback texture probe without blur", ProbeKind::InsetNoBlurTexture},
    {"15", "", "manual RenderManager inset child-layer composite without blur", ProbeKind::InsetChildCompositeTexture},
    {"16", "", "manual RenderManager inset child-layer blur without clip masks", ProbeKind::InsetBlurNoClipTexture},
    {"17", "", "manual RenderManager inset child-layer blur with inverse clip only", ProbeKind::InsetBlurInverseOnlyTexture},
};

void print_cases(const char* executable)
{
    std::fprintf(stderr, "usage: %s [case]\n", executable ? executable : "rmlui_bgfx_sample_effects_probe");
    for (const ProbeCase& probe_case : kCases) {
        std::fprintf(stderr, "  %.*s  %.*s\n", int(probe_case.key.size()), probe_case.key.data(),
                     int(probe_case.description.size()), probe_case.description.data());
    }
}

const ProbeCase* find_case(std::string_view key)
{
    for (const ProbeCase& probe_case : kCases) {
        if (probe_case.key == key || probe_case.file == key) {
            return &probe_case;
        }
    }
    return nullptr;
}

struct DirectClipMaskProbe {
    Rml::CompiledGeometryHandle background = 0;
    Rml::CompiledGeometryHandle red_shadow = 0;
    Rml::CompiledGeometryHandle blue_fill = 0;
    Rml::CompiledGeometryHandle mask = 0;
    Rml::CompiledGeometryHandle white_box = 0;

    bool valid() const
    {
        return background && red_shadow && blue_fill && mask && white_box;
    }
};

Rml::CompiledGeometryHandle compile_quad(Rml::RenderInterface& renderer, Rml::Vector2f origin,
                                         Rml::Vector2f dimensions,
                                         Rml::ColourbPremultiplied color)
{
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, origin, dimensions, color);
    return renderer.CompileGeometry(mesh.vertices, mesh.indices);
}

DirectClipMaskProbe compile_direct_clip_mask_probe(Rml::RenderInterface& renderer)
{
    DirectClipMaskProbe probe;
    probe.background = compile_quad(renderer, {0.0f, 0.0f}, {1024.0f, 768.0f},
                                    Rml::ColourbPremultiplied(164, 182, 183, 255));
    probe.red_shadow = compile_quad(renderer, {260.0f, 210.0f}, {390.0f, 160.0f},
                                    Rml::ColourbPremultiplied(255, 80, 80, 255));
    probe.blue_fill = compile_quad(renderer, {260.0f, 430.0f}, {390.0f, 160.0f},
                                   Rml::ColourbPremultiplied(80, 120, 255, 255));
    probe.mask = compile_quad(renderer, {345.0f, 245.0f}, {220.0f, 90.0f},
                              Rml::ColourbPremultiplied(255, 255, 255, 255));
    probe.white_box = compile_quad(renderer, {345.0f, 245.0f}, {220.0f, 90.0f},
                                   Rml::ColourbPremultiplied(245, 250, 250, 255));
    return probe;
}

void release_direct_clip_mask_probe(Rml::RenderInterface& renderer, DirectClipMaskProbe& probe)
{
    const Rml::CompiledGeometryHandle handles[] = {probe.background, probe.red_shadow,
                                                   probe.blue_fill, probe.mask, probe.white_box};
    for (Rml::CompiledGeometryHandle handle : handles) {
        if (handle) {
            renderer.ReleaseGeometry(handle);
        }
    }
    probe = {};
}

void render_direct_clip_mask_probe(Rml::RenderInterface& renderer, const DirectClipMaskProbe& probe)
{
    if (!probe.valid()) {
        return;
    }

    renderer.EnableScissorRegion(false);
    renderer.SetTransform(nullptr);
    renderer.EnableClipMask(false);
    renderer.RenderGeometry(probe.background, {}, {});

    // Upper block: SetInverse should keep the red quad outside the white mask and reject it
    // under the white box. This is the same primitive operation used by outer box-shadow
    // generation before RmlUi saves the callback texture.
    renderer.EnableClipMask(true);
    renderer.RenderToClipMask(Rml::ClipMaskOperation::SetInverse, probe.mask, {});
    renderer.RenderGeometry(probe.red_shadow, {}, {});
    renderer.EnableClipMask(false);
    renderer.RenderGeometry(probe.white_box, {}, {});

    // Lower block: Set is a control. The blue quad should be visible only inside the mask.
    renderer.EnableClipMask(true);
    renderer.RenderToClipMask(Rml::ClipMaskOperation::Set, probe.mask, {0.0f, 220.0f});
    renderer.RenderGeometry(probe.blue_fill, {}, {});
    renderer.EnableClipMask(false);
}

struct CallbackTextureProbe {
    Rml::Geometry textured_quad;
    Rml::CallbackTexture texture;
    bool valid = false;
};

Rml::Geometry make_textured_probe_quad(Rml::RenderManager& render_manager)
{
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, {260.0f, 180.0f}, {390.0f, 160.0f},
                                     Rml::ColourbPremultiplied(255, 255, 255, 255), {0.0f, 0.0f},
                                     {1.0f, 1.0f});
    return render_manager.MakeGeometry(std::move(mesh));
}

Rml::Geometry make_manager_quad(Rml::RenderManager& render_manager, Rml::Vector2f origin,
                               Rml::Vector2f dimensions, Rml::ColourbPremultiplied color)
{
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, origin, dimensions, color);
    return render_manager.MakeGeometry(std::move(mesh));
}

CallbackTextureProbe compile_callback_texture_probe(Rml::RenderManager& render_manager)
{
    CallbackTextureProbe probe;
    probe.textured_quad = make_textured_probe_quad(render_manager);
    probe.texture = render_manager.MakeCallbackTexture(
        [](const Rml::CallbackTextureInterface& texture_interface) -> bool {
            Rml::RenderManager& callback_render_manager = texture_interface.GetRenderManager();
            const Rml::RenderState initial_state = callback_render_manager.GetState();

            Rml::Geometry red_shadow = make_manager_quad(
                callback_render_manager, {30.0f, 35.0f}, {330.0f, 100.0f},
                Rml::ColourbPremultiplied(255, 80, 80, 255));
            Rml::Geometry mask = make_manager_quad(callback_render_manager, {85.0f, 55.0f},
                                                   {220.0f, 60.0f},
                                                   Rml::ColourbPremultiplied(255, 255, 255, 255));
            Rml::Geometry white_box = make_manager_quad(
                callback_render_manager, {85.0f, 55.0f}, {220.0f, 60.0f},
                Rml::ColourbPremultiplied(245, 250, 250, 255));

            callback_render_manager.ResetState();
            callback_render_manager.SetScissorRegion(Rml::Rectanglei::FromSize({390, 160}));
            callback_render_manager.PushLayer();

            callback_render_manager.SetClipMask(Rml::ClipMaskOperation::SetInverse, &mask, {});
            red_shadow.Render({});
            callback_render_manager.DisableClipMask();
            white_box.Render({});

            texture_interface.SaveLayerAsTexture();

            callback_render_manager.PopLayer();
            callback_render_manager.SetState(initial_state);
            return true;
        });
    probe.valid = true;
    return probe;
}

void release_callback_texture_probe(CallbackTextureProbe& probe)
{
    probe.textured_quad.Release(Rml::Geometry::ReleaseMode::ClearMesh);
    probe.texture.Release();
    probe.valid = false;
}

void render_callback_texture_probe(Rml::RenderInterface& renderer, const CallbackTextureProbe& probe)
{
    renderer.EnableScissorRegion(false);
    renderer.SetTransform(nullptr);
    renderer.EnableClipMask(false);
    if (probe.valid) {
        probe.textured_quad.Render({}, probe.texture);
    }
}

Rml::RenderBox make_probe_render_box()
{
    return Rml::RenderBox({280.0f, 70.0f}, {0.0f, 0.0f}, {2.0f, 2.0f, 2.0f, 2.0f},
                          {30.0f, 8.0f, 30.0f, 8.0f});
}

Rml::Geometry make_rounded_background_geometry(Rml::RenderManager& render_manager,
                                               Rml::ColourbPremultiplied color)
{
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateBackground(mesh, make_probe_render_box(), color);
    return render_manager.MakeGeometry(std::move(mesh));
}

Rml::Geometry make_rounded_background_border_geometry(Rml::RenderManager& render_manager)
{
    Rml::Mesh mesh;
    const Rml::ColourbPremultiplied background(222, 232, 232, 255);
    const Rml::ColourbPremultiplied border(222, 246, 247, 255);
    const Rml::Array<Rml::ColourbPremultiplied, 4> border_colors = {border, border, border, border};
    Rml::MeshUtilities::GenerateBackgroundBorder(mesh, make_probe_render_box(), background,
                                                 border_colors.data());
    return render_manager.MakeGeometry(std::move(mesh));
}

CallbackTextureProbe compile_rounded_shadow_texture_probe(Rml::RenderManager& render_manager)
{
    CallbackTextureProbe probe;
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, {260.0f, 170.0f}, {370.0f, 160.0f},
                                     Rml::ColourbPremultiplied(255, 255, 255, 255), {0.0f, 0.0f},
                                     {1.0f, 1.0f});
    probe.textured_quad = render_manager.MakeGeometry(std::move(mesh));
    probe.texture = render_manager.MakeCallbackTexture(
        [](const Rml::CallbackTextureInterface& texture_interface) -> bool {
            Rml::RenderManager& callback_render_manager = texture_interface.GetRenderManager();
            const Rml::RenderState initial_state = callback_render_manager.GetState();

            Rml::Geometry background_border =
                make_rounded_background_border_geometry(callback_render_manager);
            Rml::Geometry mask = make_rounded_background_geometry(
                callback_render_manager, Rml::ColourbPremultiplied(255, 255, 255, 255));
            Rml::Geometry red_shadow = make_rounded_background_geometry(
                callback_render_manager, Rml::ColourbPremultiplied(255, 102, 102, 255));
            Rml::Geometry rose_shadow = make_rounded_background_geometry(
                callback_render_manager, Rml::ColourbPremultiplied(204, 136, 136, 255));
            Rml::Geometry tan_shadow = make_rounded_background_geometry(
                callback_render_manager, Rml::ColourbPremultiplied(187, 170, 170, 255));

            callback_render_manager.ResetState();
            callback_render_manager.SetScissorRegion(Rml::Rectanglei::FromSize({370, 160}));
            callback_render_manager.PushLayer();

            background_border.Render({0.0f, 0.0f});
            callback_render_manager.SetClipMask(Rml::ClipMaskOperation::SetInverse, &mask,
                                                {0.0f, 0.0f});
            tan_shadow.Render({90.0f, 90.0f});
            callback_render_manager.SetClipMask(Rml::ClipMaskOperation::SetInverse, &mask,
                                                {0.0f, 0.0f});
            rose_shadow.Render({60.0f, 60.0f});
            callback_render_manager.SetClipMask(Rml::ClipMaskOperation::SetInverse, &mask,
                                                {0.0f, 0.0f});
            red_shadow.Render({30.0f, 30.0f});

            texture_interface.SaveLayerAsTexture();

            callback_render_manager.PopLayer();
            callback_render_manager.SetState(initial_state);
            return true;
        });
    probe.valid = true;
    return probe;
}

Rml::Geometry make_inset_shadow_geometry(Rml::RenderManager& render_manager,
                                         Rml::ColourbPremultiplied color, float spread_distance)
{
    Rml::RenderBox render_box = make_probe_render_box();
    Rml::CornerSizes spread_radii = render_box.GetBorderRadius();
    for (float& radius : spread_radii) {
        float spread_factor = -1.0f;
        if (radius < spread_distance) {
            const float ratio_minus_one = (radius / spread_distance) - 1.0f;
            spread_factor *= 1.0f + ratio_minus_one * ratio_minus_one * ratio_minus_one;
        }
        radius = Rml::Math::Max(radius + spread_factor * spread_distance, 0.0f);
    }

    const float signed_spread_distance = -spread_distance;
    render_box.SetFillSize(Rml::Math::Max(
        render_box.GetFillSize() + Rml::Vector2f(2.0f * signed_spread_distance),
        Rml::Vector2f{0.001f}));
    render_box.SetBorderRadius(spread_radii);
    render_box.SetBorderOffset(render_box.GetBorderOffset() -
                               Rml::Vector2f(signed_spread_distance));

    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateBackground(mesh, render_box, color);
    return render_manager.MakeGeometry(std::move(mesh));
}

enum class InsetClipMode {
    FullInset,
    NoClip,
    InverseOnly,
};

CallbackTextureProbe compile_inset_texture_probe(Rml::RenderManager& render_manager,
                                                bool use_child_layer, bool blur_enabled,
                                                InsetClipMode clip_mode = InsetClipMode::FullInset)
{
    CallbackTextureProbe probe;
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, {220.0f, 180.0f}, {390.0f, 160.0f},
                                     Rml::ColourbPremultiplied(255, 255, 255, 255), {0.0f, 0.0f},
                                     {1.0f, 1.0f});
    probe.textured_quad = render_manager.MakeGeometry(std::move(mesh));
    probe.texture = render_manager.MakeCallbackTexture(
        [use_child_layer, blur_enabled, clip_mode](const Rml::CallbackTextureInterface& texture_interface) -> bool {
            Rml::RenderManager& callback_render_manager = texture_interface.GetRenderManager();
            const Rml::RenderState initial_state = callback_render_manager.GetState();
            const Rml::Vector2f box_offset{55.0f, 45.0f};
            const Rml::ColourbPremultiplied shadow_color(255, 102, 255, 204);

            Rml::Geometry background_border =
                make_rounded_background_border_geometry(callback_render_manager);
            Rml::Geometry padding =
                make_rounded_background_geometry(callback_render_manager, shadow_color);
            Rml::Geometry inset_shadow =
                make_inset_shadow_geometry(callback_render_manager, shadow_color, 14.0f);

            callback_render_manager.ResetState();
            callback_render_manager.SetScissorRegion(Rml::Rectanglei::FromSize({390, 160}));
            callback_render_manager.PushLayer();
            background_border.Render(box_offset);

            Rml::CompiledFilter blur;
            if (blur_enabled) {
                blur = callback_render_manager.CompileFilter(
                    "blur", Rml::Dictionary{{"sigma", Rml::Variant(10.0f)}});
            }
            if (use_child_layer) {
                callback_render_manager.PushLayer();
            }

            if (clip_mode == InsetClipMode::FullInset || clip_mode == InsetClipMode::InverseOnly) {
                callback_render_manager.SetClipMask(Rml::ClipMaskOperation::SetInverse,
                                                    &inset_shadow, box_offset);
            }
            padding.Render(box_offset);
            if (clip_mode == InsetClipMode::FullInset) {
                callback_render_manager.SetClipMask(Rml::ClipMaskOperation::Set, &padding,
                                                    box_offset);
            }

            if (use_child_layer) {
                Rml::FilterHandleList filters;
                if (blur) {
                    blur.AddHandleTo(filters);
                }
                callback_render_manager.CompositeLayers(callback_render_manager.GetTopLayer(),
                                                        callback_render_manager.GetNextLayer(),
                                                        Rml::BlendMode::Blend, filters);
                callback_render_manager.PopLayer();
            }
            if (blur) {
                blur.Release();
            }

            texture_interface.SaveLayerAsTexture();
            callback_render_manager.PopLayer();
            callback_render_manager.SetState(initial_state);
            return true;
        });
    probe.valid = true;
    return probe;
}

CallbackTextureProbe compile_inset_blur_texture_probe(Rml::RenderManager& render_manager)
{
    return compile_inset_texture_probe(render_manager, true, true);
}

CallbackTextureProbe compile_inset_no_blur_texture_probe(Rml::RenderManager& render_manager)
{
    return compile_inset_texture_probe(render_manager, false, false);
}

CallbackTextureProbe compile_inset_child_composite_texture_probe(Rml::RenderManager& render_manager)
{
    return compile_inset_texture_probe(render_manager, true, false);
}

CallbackTextureProbe compile_inset_blur_no_clip_texture_probe(Rml::RenderManager& render_manager)
{
    return compile_inset_texture_probe(render_manager, true, true, InsetClipMode::NoClip);
}

CallbackTextureProbe compile_inset_blur_inverse_only_texture_probe(Rml::RenderManager& render_manager)
{
    return compile_inset_texture_probe(render_manager, true, true, InsetClipMode::InverseOnly);
}

int run(int argc, char** argv)
{
    const ProbeCase* probe_case = &kCases[0];
    if (argc > 1) {
        const std::string_view arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            print_cases(argv[0]);
            return 0;
        }
        probe_case = find_case(arg);
        if (!probe_case) {
            std::fprintf(stderr, "Unknown effects probe case: %.*s\n", int(arg.size()), arg.data());
            print_cases(argv[0]);
            return 2;
        }
    }

    constexpr int window_width = 1024;
    constexpr int window_height = 768;

    if (!Shell::Initialize()) {
        return -1;
    }

    if (!Backend::Initialize("Effects Probe", window_width, window_height, true)) {
        Shell::Shutdown();
        return -1;
    }

    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());

    Rml::Initialise();
    Rml::Context* context = Rml::CreateContext("main", Rml::Vector2i(window_width, window_height));
    if (!context) {
        Rml::Shutdown();
        Backend::Shutdown();
        Shell::Shutdown();
        return -1;
    }

    Rml::Debugger::Initialise(context);

    // Fonts are loaded through the upstream sample shell file interface. Afterward, swap to a
    // deterministic local file interface so every probe case is owned by this repository.
    Shell::LoadFonts();
    ProbeFileInterface file_interface(RMLUI_BGFX_EFFECTS_PROBE_DATA_DIR);
    Rml::SetFileInterface(&file_interface);

    Rml::RenderInterface* renderer = Backend::GetRenderInterface();
    DirectClipMaskProbe direct_probe;
    CallbackTextureProbe callback_probe;
    if (probe_case->kind == ProbeKind::DirectClipMask) {
        if (!renderer) {
            std::fprintf(stderr, "Direct renderer probe requires a render interface.\n");
        } else {
            direct_probe = compile_direct_clip_mask_probe(*renderer);
            if (!direct_probe.valid()) {
                std::fprintf(stderr, "Failed to compile direct renderer probe geometry.\n");
            }
        }
    } else if (probe_case->kind == ProbeKind::CallbackTexture) {
        callback_probe = compile_callback_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::RoundedShadowTexture) {
        callback_probe = compile_rounded_shadow_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::InsetBlurTexture) {
        callback_probe = compile_inset_blur_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::InsetNoBlurTexture) {
        callback_probe = compile_inset_no_blur_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::InsetChildCompositeTexture) {
        callback_probe = compile_inset_child_composite_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::InsetBlurNoClipTexture) {
        callback_probe = compile_inset_blur_no_clip_texture_probe(context->GetRenderManager());
    } else if (probe_case->kind == ProbeKind::InsetBlurInverseOnlyTexture) {
        callback_probe = compile_inset_blur_inverse_only_texture_probe(context->GetRenderManager());
    } else if (Rml::ElementDocument* document = context->LoadDocument(probe_case->file.data())) {
        document->Show();
    } else {
        std::fprintf(stderr, "Failed to load effects probe case: %.*s\n",
                     int(probe_case->file.size()), probe_case->file.data());
    }

    bool running = true;
    while (running) {
        running = Backend::ProcessEvents(context, &Shell::ProcessKeyDownShortcuts, false);
        context->Update();
        Backend::BeginFrame();
        if (probe_case->kind == ProbeKind::DirectClipMask && renderer) {
            render_direct_clip_mask_probe(*renderer, direct_probe);
        } else if ((probe_case->kind == ProbeKind::CallbackTexture ||
                    probe_case->kind == ProbeKind::RoundedShadowTexture ||
                    probe_case->kind == ProbeKind::InsetBlurTexture ||
                    probe_case->kind == ProbeKind::InsetNoBlurTexture ||
                    probe_case->kind == ProbeKind::InsetChildCompositeTexture ||
                    probe_case->kind == ProbeKind::InsetBlurNoClipTexture ||
                    probe_case->kind == ProbeKind::InsetBlurInverseOnlyTexture) &&
                   renderer) {
            render_callback_texture_probe(*renderer, callback_probe);
        } else {
            context->Render();
        }
        Backend::PresentFrame();
    }

    if (renderer) {
        release_direct_clip_mask_probe(*renderer, direct_probe);
    }
    release_callback_texture_probe(callback_probe);
    Rml::Shutdown();
    Backend::Shutdown();
    Shell::Shutdown();
    return 0;
}

} // namespace

#if defined RMLUI_PLATFORM_WIN32
int APIENTRY WinMain(HINSTANCE /*instance_handle*/, HINSTANCE /*previous_instance_handle*/,
                     char* /*command_line*/, int /*command_show*/)
{
    return run(__argc, __argv);
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
