#include "weft/engine/plugins/Vst3Host.h"

#include "weft/engine/plugins/HostedPlugin.h"

#include <pluginterfaces/base/ipluginbase.h>

#include <map>
#include <memory>

#if JUCE_WINDOWS
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#if JUCE_LINUX || JUCE_BSD
    #include <sys/utsname.h>
#endif

namespace weft::engine
{
namespace
{
/** The binary inside a .vst3 bundle (or the file itself, for the single-file layout). */
juce::File moduleBinary (const std::filesystem::path& path)
{
    const juce::File bundle (juce::String (path.string()));
#if JUCE_LINUX || JUCE_BSD
    utsname name {};
    const juce::String machine = uname (&name) == 0 ? juce::String (name.machine) : juce::String ("x86_64");
    return bundle.getChildFile ("Contents").getChildFile (machine + "-linux").getChildFile (bundle.getFileNameWithoutExtension() + ".so");
#else
    if (bundle.isDirectory())
        return bundle.getChildFile ("Contents").getChildFile ("x86_64-win").getChildFile (bundle.getFileName());
    return bundle;
#endif
}

std::string hexUid (const Steinberg::TUID cid)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (int i = 0; i < 16; ++i)
    {
        const auto b = static_cast<unsigned char> (cid[i]);
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}
} // namespace

std::vector<Vst3Class> readVst3Classes (const std::filesystem::path& file, std::string* error)
{
    std::vector<Vst3Class> out;
    const auto binary = moduleBinary (file);
    // The module stays loaded: JUCE may load it too, and unloading under it is unsafe.
    using GetFactory = Steinberg::IPluginFactory* (PLUGIN_API*) ();
#if JUCE_WINDOWS
    auto* module = LoadLibraryW (binary.getFullPathName().toWideCharPointer());
    const auto symbol = [module] (const char* name) { return reinterpret_cast<void*> (GetProcAddress (module, name)); };
#else
    auto* module = dlopen (binary.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    const auto symbol = [module] (const char* name) { return dlsym (module, name); };
#endif
    if (module == nullptr)
    {
        if (error != nullptr)
            *error = "Couldn't load " + binary.getFullPathName().toStdString();
        return out;
    }
#if JUCE_WINDOWS
    if (auto* entry = reinterpret_cast<bool (*)()> (symbol ("InitDll")))
        entry();
#elif JUCE_LINUX || JUCE_BSD
    if (auto* entry = reinterpret_cast<bool (*) (void*)> (symbol ("ModuleEntry")))
        entry (module);
#endif
    auto* getFactory = reinterpret_cast<GetFactory> (symbol ("GetPluginFactory"));
    Steinberg::IPluginFactory* factory = getFactory != nullptr ? getFactory() : nullptr;
    if (factory == nullptr)
    {
        if (error != nullptr)
            *error = "Not a VST3 plug-in (no factory)";
        return out;
    }
    for (Steinberg::int32 i = 0; i < factory->countClasses(); ++i)
    {
        Steinberg::PClassInfo info {};
        if (factory->getClassInfo (i, &info) != Steinberg::kResultOk)
            continue;
        if (std::string (info.category) != "Audio Module Class")
            continue;
        out.push_back ({ hexUid (info.cid), info.name, info.category });
    }
    factory->release();
    return out;
}

struct Vst3Host::Impl
{
    juce::VST3PluginFormat format;
    std::map<std::string, juce::OwnedArray<juce::PluginDescription>> types; // by file

    const juce::PluginDescription* describe (const std::filesystem::path& file, const std::string& name)
    {
        auto& list = types[file.string()];
        if (list.isEmpty())
            format.findAllTypesForFile (list, juce::String (file.string()));
        for (auto* d : list)
            if (d->name.toStdString() == name)
                return d;
        return list.size() == 1 ? list[0] : nullptr;
    }

    std::shared_ptr<Processor> load (const std::filesystem::path& file, const std::string& name, std::string* error)
    {
        const auto* desc = describe (file, name);
        if (desc == nullptr)
        {
            if (error != nullptr)
                *error = name + " isn't in " + file.string();
            return nullptr;
        }
        juce::String message;
        auto instance = format.createInstanceFromDescription (*desc, 48000.0, 512, message);
        if (! instance)
        {
            if (error != nullptr)
                *error = message.toStdString();
            return nullptr;
        }
        return std::make_shared<HostedPlugin> (std::move (instance));
    }
};

Vst3Host::Vst3Host (const PluginCatalog& c) : impl (std::make_unique<Impl>()), catalog (c) {}
Vst3Host::~Vst3Host() = default;

std::shared_ptr<Processor> Vst3Host::create (const model::PluginInstance& plugin, bool, std::string* error)
{
    if (plugin.format != "vst3")
        return nullptr;
    const auto* info = catalog.find (plugin.pluginUid);
    if (info == nullptr)
    {
        if (error != nullptr)
            *error = plugin.name + " isn't installed on this computer.";
        return nullptr;
    }
    return impl->load (info->file, info->name, error);
}

std::shared_ptr<Processor> Vst3Host::createFromFile (const std::filesystem::path& file, const std::string& name, std::string* error)
{
    return impl->load (file, name, error);
}
} // namespace weft::engine
