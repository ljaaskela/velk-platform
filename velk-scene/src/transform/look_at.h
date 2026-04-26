#ifndef VELK_UI_LOOK_AT_TRANSFORM_H
#define VELK_UI_LOOK_AT_TRANSFORM_H

#include <velk/api/change.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_look_at.h>
#include <velk-scene/plugin.h>

namespace velk {

class LookAt : public ext::Transform<LookAt, ILookAt>
{
public:
    VELK_CLASS_UID(ClassId::Transform::LookAt, "LookAt");

    void transform(IElement& element) override;

private:
    struct CacheKey
    {
        IElement* target{};
        vec3 target_pos{};
        size target_size{};
        vec3 target_offset{};
        vec3 eye_pos{};

        bool operator==(const CacheKey& o) const
        {
            return target == o.target
                && target_pos == o.target_pos
                && target_size == o.target_size
                && target_offset == o.target_offset
                && eye_pos == o.eye_pos;
        }
        bool operator!=(const CacheKey& o) const { return !(*this == o); }
    };
    ChangeCache<CacheKey> cache_;
};

} // namespace velk

#endif // VELK_UI_LOOK_AT_TRANSFORM_H
