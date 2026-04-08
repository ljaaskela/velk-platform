#include "font_gpu_buffer.h"

#include <velk/ext/core_object.h>

namespace velk::ui {

::velk::IBuffer::Ptr FontGpuBuffer::make(FontBuffers* fb, Role role)
{
    // make_object<T> stores the raw T* via void* indirection (see
    // velk/ext/core_object.h), so the IObject* in the resulting Ptr already
    // points to the start of the FontGpuBuffer object. Round-trip through
    // void* mirrors that pattern and avoids any compiler-applied offset
    // adjustments that a direct static_cast<IObject*>->FontGpuBuffer* would
    // introduce on a different layout.
    auto raw = ::velk::ext::make_object<FontGpuBuffer>();
    auto* fgb = static_cast<FontGpuBuffer*>(static_cast<void*>(raw.get()));
    fgb->init(fb, role);
    return interface_pointer_cast<::velk::IBuffer>(raw);
}

} // namespace velk::ui
