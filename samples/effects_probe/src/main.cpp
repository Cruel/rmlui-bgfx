#include <RmlUi/Core.h>
#include <RmlUi/Core/FileInterface.h>
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

struct ProbeCase {
    std::string_view key;
    std::string_view file;
    std::string_view description;
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

    if (Rml::ElementDocument* document = context->LoadDocument(probe_case->file.data())) {
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
