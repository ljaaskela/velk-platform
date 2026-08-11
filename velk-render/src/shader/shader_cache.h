#ifndef VELK_RENDER_SHADER_CACHE_H
#define VELK_RENDER_SHADER_CACHE_H

#include <velk/string.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk {

/**
 * @brief On-disk cache for compiled SPIR-V shaders.
 *
 * Cached blobs are stored under shader_cache:// (a FileProtocol pointing at
 * <cwd>/shader_cache/). Per-shader cache keys are computed by the caller and
 * should already incorporate the source hash, the stage, and a hash of the
 * shader includes used at compile time, so that any change to a virtual
 * include automatically invalidates affected entries (old entries become
 * orphaned, not corrupt).
 *
 * The cache can be disabled at runtime via the VELK_SHADER_CACHE_DISABLED
 * environment variable.
 */
class ShaderCache
{
public:
    /**
     * @brief Lazily initializes the cache.
     *
     * Idempotent. Registers the shader_cache:// protocol and ensures the
     * cache directory exists. Honors VELK_SHADER_CACHE_DISABLED.
     */
    void ensure_initialized();

    /** @brief Returns true if the cache is enabled and initialized. */
    bool enabled() const { return enabled_ && initialized_; }

    /**
     * @brief Reads cached SPIR-V for @p key.
     * @return The cached SPIR-V words, or an empty vector on miss.
     */
    vector<uint32_t> read(uint64_t key) const;

    /**
     * @brief Writes @p spirv to the cache under @p key. Best-effort; failures
     * are logged but do not propagate.
     */
    void write(uint64_t key, const vector<uint32_t>& spirv) const;

private:
    bool enabled_ = true;
    bool initialized_ = false;
    string cache_dir_; ///< Filesystem path to the cache directory; ends in '/'.
};

} // namespace velk

#endif // VELK_RENDER_SHADER_CACHE_H
