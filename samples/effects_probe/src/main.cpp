#include <RmlUi/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Backend.h>
#include <Shell.h>

#include <cstdio>
#include <string>
#include <string_view>

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

struct ProbeCase {
    std::string_view key;
    std::string_view file;
    std::string_view description;
};

constexpr ProbeCase kCases[] = {
    {"00", "00_shader_time.rml", "u_rmluiMaterialParams0.x elapsed time animation"},
    {"01", "01_shader_dimensions.rml", "u_rmluiMaterialParams0.yz paint width and height"},
    {"02", "02_shader_dpi.rml", "u_rmluiMaterialParams0.w DPI/content scale"},
    {"03", "03_shader_mouse.rml", "u_rmluiMaterialParams1.xy mouse coordinates and validity"},
    {"04", "04_shader_combined.rml", "combined time, dimensions, DPI, and mouse standard uniforms"},
    {"05", "05_scroll_root_filter.rml", "scrolling root with top effect stack"},
    {"06", "06_scroll_only.rml", "nested scroller without transforms or filters"},
    {"07", "07_scroll_transform_only.rml", "nested scroller with transforms only"},
    {"08", "08_scroll_filter_only.rml", "nested scroller with filters only"},
    {"09", "09_scroll_shadow_only.rml", "nested scroller with box shadows only"},
    {"10", "10_scroll_transform_shadow.rml", "nested scroller with transform plus box-shadow"},
    {"11", "11_scroll_transform_filter.rml", "nested scroller with transform plus filter"},
    {"12", "12_scroll_transform_shadow_filter.rml", "nested scroller with transform plus box-shadow plus filter"},
    {"13", "13_effects_scroll_baseline.rml", "stripped effects layout with plain scrolling"},
    {"14", "14_effects_noop_filter_scroll.rml", "effects layout with no-op outer filter and scrolling"},
    {"15", "15_effects_transform_filter_scroll.rml", "effects layout with transforms plus no-op outer filter"},
    {"16", "16_effects_shadow_overflow_scroll.rml", "effects layout with box-shadow overflow in filtered wrapper"},
    {"17", "17_effects_real_filters_scroll.rml", "effects layout with per-item real filters while scrolling"},
    {"18", "18_effects_outer_blur_scroll.rml", "effects layout with real outer blur while scrolling"},
    {"19", "19_effects_outer_dropshadow_scroll.rml", "effects layout with real outer drop-shadow while scrolling"},
    {"20", "20_effects_backdrop_scroll.rml", "effects layout with backdrop-filter window over scrolling content"},
    {"21", "21_effects_scroll_mid_bottom.rml", "effects layout with explicit mid and bottom scroll markers"},
    {"22", "22_effects_no_wrapper_filter_control.rml", "effects layout control without the outer wrapper filter"},
    {"23", "23_effects_stripped_full_slice.rml", "stripped full effects sample slice with scrolling"},
    {"24", "24_effects_soft_shadow_filtered.rml", "single full-sample blur box-shadow inside filtered wrapper"},
    {"25", "25_effects_soft_shadow_unfiltered.rml", "single full-sample blur box-shadow without filtered wrapper"},
    {"26", "26_effects_trail_shadow_filtered.rml", "full-sample trail box-shadow inside filtered wrapper"},
    {"27", "27_effects_trail_shadow_unfiltered.rml", "full-sample trail box-shadow without filtered wrapper"},
    {"28", "28_effects_soft_shadow_no_transform_filtered.rml",
     "full-sample blur box-shadow without transform inside filtered wrapper"},
    {"29", "29_effects_inset_only.rml", "single inset box-shadow without outer shadows"},
    {"30", "30_effects_inset_no_blur.rml", "single inset box-shadow without blur"},
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
            std::fprintf(stderr, "Unknown shader probe case: %.*s\n", int(arg.size()), arg.data());
            print_cases(argv[0]);
            return 2;
        }
    }

    constexpr int window_width = 1024;
    constexpr int window_height = 768;

    if (!Shell::Initialize()) {
        return -1;
    }

    if (!Backend::Initialize("Shader Uniform Probe", window_width, window_height, true)) {
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

    Shell::LoadFonts();
    ProbeFileInterface file_interface(RMLUI_BGFX_EFFECTS_PROBE_DATA_DIR);
    Rml::SetFileInterface(&file_interface);

    if (Rml::ElementDocument* document = context->LoadDocument(probe_case->file.data())) {
        document->Show();
    } else {
        std::fprintf(stderr, "Failed to load shader probe case: %.*s\n",
                     int(probe_case->file.size()), probe_case->file.data());
    }

    bool running = true;
    while (running) {
        running = Backend::ProcessEvents(context, &Shell::ProcessKeyDownShortcuts, false);
        context->Update();
        Backend::BeginFrame();
        context->Render();
        Backend::PresentFrame();
    }

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
