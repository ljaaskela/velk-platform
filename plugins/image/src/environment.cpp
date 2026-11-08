#include "environment.h"

#include "env_material.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

namespace velk::ui::impl {

void Environment::init_material()
{
    material_ = ::velk::instance().create<IMaterial>(ClassId::Material::Environment);
    sync_material();
}

void Environment::sync_material()
{
    if (auto m = interface_cast<IEnvMaterialInternal>(material_)) {
        if (auto state = read_state<IEnvironment>(this)) {
            m->set_params(state->intensity, state->rotation);
        }
    }
}

void Environment::init(string_view uri, int width, int height, vector<uint8_t> pixels)
{
    uri_ = string(uri);
    width_ = width;
    height_ = height;
    pixels_ = std::move(pixels);
    dirty_ = true;
    init_material();
}

} // namespace velk::ui::impl
