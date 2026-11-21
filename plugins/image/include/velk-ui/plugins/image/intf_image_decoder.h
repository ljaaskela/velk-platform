#ifndef VELK_UI_INTF_IMAGE_DECODER_H
#define VELK_UI_INTF_IMAGE_DECODER_H

#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>
#include <velk-render/render_types.h>

namespace velk::ui {

/**
 * @brief The image decoder (registered as "image"), told what the device samples.
 *
 * A Basis Universal KTX2 file is transcoded to the best block format among those, and an ASTC
 * or ETC file the device cannot sample is decoded to RGBA8. Until it is told, it assumes the
 * formats devices of its platform commonly sample. Images already decoding keep what they had.
 */
class IImageDecoder : public Interface<IImageDecoder>
{
public:
    /** @brief Sets the block compressed formats the device samples. */
    virtual void set_sampled_formats(const vector<PixelFormat>& formats) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_IMAGE_DECODER_H
