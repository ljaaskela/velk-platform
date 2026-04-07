#include "image_plugin.h"

#include "image.h"
#include "image_decoder.h"
#include "image_material.h"
#include "image_visual.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource_store.h>

namespace velk::ui::impl {

ReturnValue ImagePlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<Image>(velk);
    rv &= register_type<ImageDecoder>(velk);
    rv &= register_type<ImageMaterial>(velk);
    rv &= register_type<ImageVisual>(velk);

    // Register the decoder with the resource store so URIs of the form
    // "image:<inner_uri>" route through it.
    auto obj = velk.create<IResourceDecoder>(ImageDecoder::static_class_id());
    if (!obj) {
        return ReturnValue::Fail;
    }
    decoder_ = obj;
    velk.resource_store().register_decoder(decoder_);

    return rv;
}

ReturnValue ImagePlugin::shutdown(IVelk& velk)
{
    if (decoder_) {
        velk.resource_store().unregister_decoder(decoder_);
        decoder_ = nullptr;
    }
    return ReturnValue::Success;
}

} // namespace velk::ui::impl
