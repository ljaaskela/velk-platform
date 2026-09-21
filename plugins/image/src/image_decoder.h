#ifndef VELK_UI_IMAGE_DECODER_H
#define VELK_UI_IMAGE_DECODER_H

#include <velk/api/task_pool.h>
#include <velk/ext/object.h>
#include <velk/interface/resource/intf_resource_decoder.h>

#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Resource decoder that turns raw image bytes (png, jpg, bmp, ...)
 *        into `Image` objects.
 *
 * Registered with the resource store under the name "image". Apps fetch
 * decoded images via `instance().resource_store().get_resource<IImage>(
 *     "image:app://path/to/file.png")`.
 *
 * Decoding is asynchronous. decode() returns an `Image` in the Loading
 * state right away; a worker reads and decodes the bytes, and the result is
 * handed back to the image in drain_commits(), which the image plugin calls
 * from its pre_update(). Both pools are owned by the decoder, so releasing
 * the decoder joins the workers and drops pending work.
 */
class ImageDecoder final : public ::velk::ext::Object<ImageDecoder, IResourceDecoder>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::ImageDecoder, "ImageDecoder");

    ImageDecoder();

    string_view name() const override { return "image"; }
    IResource::Ptr decode(const IResource::Ptr& inner) const override;

    /// Hands decoded results back to their images. Call on the main thread.
    void drain_commits();

private:
    // Declared first so it is destroyed last: decode_pool_ joins its workers,
    // which post into this pool, before it goes away.
    mutable ManualTaskPool commit_pool_;   ///< Hand-backs, run by drain_commits().
    mutable ThreadedTaskPool decode_pool_; ///< Reads and decodes bytes.
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_DECODER_H
