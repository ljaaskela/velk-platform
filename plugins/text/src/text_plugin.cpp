#include "text_plugin.h"

#include "embedded/inter_regular.h"
#include "embedded/jetbrains_mono_regular.h"
#include "font.h"
#include "font_decoder.h"
#include "visual/text_field.h"
#include "visual/text_material.h"
#include "visual/text_visual.h"
#include "visual/tree_view.h"

#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>

namespace velk::ui {

namespace {

struct EmbeddedFont
{
    string_view path;
    const unsigned char* data;
    const uint32_t& size;
};

const EmbeddedFont embedded_fonts[] = {
    {"velk-text/Inter-Regular.ttf", embedded::inter_regular_ttf, embedded::inter_regular_ttf_size},
    {"velk-text/JetBrainsMono-Regular.ttf", embedded::jetbrains_mono_regular_ttf,
     embedded::jetbrains_mono_regular_ttf_size},
};

constexpr string_view default_font_uri = "font:memory://velk-text/Inter-Regular.ttf";
constexpr string_view monospace_font_uri = "font:memory://velk-text/JetBrainsMono-Regular.ttf";

} // namespace

ReturnValue TextPlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<impl::Font>(velk);
    rv &= register_type<impl::FontDecoder>(velk);
    rv &= register_type<TextMaterial>(velk);
    rv &= register_type<TextVisual>(velk);
    rv &= register_type<TextField>(velk);
    rv &= register_type<TreeView>(velk);

    auto& store = velk.resource_store();

    font_decoder_ = velk.create<IResourceDecoder>(impl::FontDecoder::static_class_id());
    if (!font_decoder_) {
        return ReturnValue::Fail;
    }
    store.register_decoder(font_decoder_);

    auto* memory = interface_cast<IMemoryProtocol>(store.find_protocol("memory").get());
    if (!memory) {
        return ReturnValue::Fail;
    }
    for (auto& f : embedded_fonts) {
        memory->add_file(f.path, f.data, f.size);
    }

    default_font_ = store.get_resource<IFont>(default_font_uri);
    monospace_font_ = store.get_resource<IFont>(monospace_font_uri);

    return rv;
}

ReturnValue TextPlugin::shutdown(IVelk& velk)
{
    default_font_.reset();
    monospace_font_.reset();

    auto& store = velk.resource_store();
    if (font_decoder_) {
        store.unregister_decoder(font_decoder_);
        font_decoder_ = nullptr;
    }
    if (auto* memory = interface_cast<IMemoryProtocol>(store.find_protocol("memory").get())) {
        for (auto& f : embedded_fonts) {
            memory->remove_file(f.path);
        }
    }
    return ReturnValue::Success;
}

IFont::Ptr TextPlugin::get_default_font() const
{
    return default_font_;
}

IFont::Ptr TextPlugin::get_monospace_font() const
{
    return monospace_font_;
}

} // namespace velk::ui
