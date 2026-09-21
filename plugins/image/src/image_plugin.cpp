#include "image_plugin.h"

#include "env_decoder.h"
#include "env_material.h"
#include "environment.h"
#include "image.h"
#include "image_decoder.h"
#include "image_encoder.h"
#include "image_material.h"
#include "image_visual.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource_store.h>

namespace velk::ui::impl {

ReturnValue ImagePlugin::initialize(IVelk& velk, PluginConfig& config)
{
    // pre_update hands asynchronously decoded images back on the main thread.
    config.enableUpdate = true;

    auto rv = register_type<Image>(velk);
    rv &= register_type<ImageDecoder>(velk);
    rv &= register_type<ImageEncoder>(velk);
    rv &= register_type<ImageMaterial>(velk);
    rv &= register_type<ImageVisual>(velk);
    rv &= register_type<Environment>(velk);
    rv &= register_type<EnvDecoder>(velk);
    rv &= register_type<EnvMaterial>(velk);

    auto& store = velk.resource_store();

    // Register decoders so URIs route through them:
    //   "image:<inner_uri>" -> ImageDecoder
    //   "env:<inner_uri>"   -> EnvDecoder
    auto img_dec = velk.create<IResourceDecoder>(ImageDecoder::static_class_id());
    if (!img_dec) {
        return ReturnValue::Fail;
    }
    image_decoder_ = img_dec;
    store.register_decoder(image_decoder_);

    auto env_dec = velk.create<IResourceDecoder>(EnvDecoder::static_class_id());
    if (!env_dec) {
        return ReturnValue::Fail;
    }
    env_decoder_ = env_dec;
    store.register_decoder(env_decoder_);

    return rv;
}

ReturnValue ImagePlugin::shutdown(IVelk& velk)
{
    auto& store = velk.resource_store();
    if (image_decoder_) {
        store.unregister_decoder(image_decoder_);
        // Releasing the decoder joins its decode workers and drops pending
        // hand-backs, so no task outlives this library.
        image_decoder_ = nullptr;
    }
    if (env_decoder_) {
        store.unregister_decoder(env_decoder_);
        env_decoder_ = nullptr;
    }
    return ReturnValue::Success;
}

void ImagePlugin::pre_update(const IPlugin::PreUpdateInfo&)
{
    if (image_decoder_) {
        static_cast<ImageDecoder*>(image_decoder_.get())->drain_commits();
    }
}

} // namespace velk::ui::impl
