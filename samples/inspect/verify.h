#ifndef VELK_INSPECT_VERIFY_H
#define VELK_INSPECT_VERIFY_H

#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/vector.h>
#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_render_backend.h>

namespace inspect {

/// An image as shown, three floats a pixel from 0 to 1, in rows from the top.
struct Image
{
    uint32_t width = 0;
    uint32_t height = 0;
    velk::vector<float> rgb;
};

/// Reads back what the window shows now. False when it cannot.
bool grab_window(velk::IRenderBackend& backend, uint64_t surface_id, Image& out);

/// @p image, each @p factor by @p factor pixels averaged into one.
Image downscale(const Image& image, uint32_t factor);

/// How far @p ours is from @p reference, in luminance: the mean and the 95th percentile of the
/// difference over pixels, and the mean over the worst square of @p tile by @p tile pixels, so
/// that a fault in one place shows however small it is against the whole. Both the same size.
struct Difference
{
    float mean = 0.f;
    float p95 = 0.f;
    float worst_tile = 0.f;
    uint32_t tile_x = 0;  ///< Where the worst square is, in squares.
    uint32_t tile_y = 0;
};
Difference compare(const Image& ours, const Image& reference, uint32_t tile);

/// How much changed between two frames of a still view, in luminance: the mean change and the
/// share of pixels changing by more than @p threshold.
struct Change
{
    float mean = 0.f;
    float share = 0.f;
};
Change frame_change(const Image& before, const Image& after, float threshold);

/// A map of where @p ours is brighter (red) or darker (blue) than @p reference, by how much.
Image difference_map(const Image& ours, const Image& reference);

/// A reference image in its file (width, height, then the floats), and back.
bool save_reference(const Image& image, velk::string_view path);
bool load_reference(velk::string_view path, Image& out);

/// @p image as a PNG at @p path.
bool save_png(const velk::IImageWriter& writer, const Image& image, velk::string_view path);

} // namespace inspect

#endif // VELK_INSPECT_VERIFY_H
