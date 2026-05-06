#ifndef VELK_RENDER_INTF_RENDER_STATE_H
#define VELK_RENDER_INTF_RENDER_STATE_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <cstdint>

namespace velk {

/**
 * @brief Bitfield describing which aspects of an `IRenderState` changed.
 *
 * Producers may OR specific bits to indicate a partial change;
 * consumers may filter and skip work. `All` means "treat everything
 * as dirty".
 */
enum class RenderStateChange : uint32_t
{
    None = 0,
    All  = 0xffffffffu,
};

class IRenderState;

/**
 * @brief Observer of an `IRenderState`'s change events.
 *
 * Lifetime contract:
 * - Notifications run on whichever thread mutated the source.
 *   Implementations must be thread-safe.
 * - @p source is valid only for the duration of the call.
 * - Observers must call `remove_render_state_observer` before they are
 *   destroyed. The source does not auto-detach observers in its
 *   destructor.
 *
 * Chain: IInterface -> IRenderStateObserver
 */
class IRenderStateObserver
    : public Interface<IRenderStateObserver, IInterface,
                       VELK_UID("25567493-6b14-4b13-99e8-3b2adf66b167")>
{
public:
    /**
     * @brief Called after the source's state has been mutated.
     *
     * Synchronous. The observer may call into @p source (including
     * `remove_render_state_observer`), but must not call
     * `add_render_state_observer` on the same source from inside the
     * callback.
     */
    virtual void on_render_state_changed(IRenderState* source,
                                         RenderStateChange flags) = 0;
};

/**
 * @brief Observable carrier for render-side state changes.
 *
 * Implemented by objects whose contents change frame-to-frame and
 * whose consumers want notification rather than per-frame diffing
 * (`IBatch`, `IRenderPass`, `IRenderGraph`).
 *
 * Separate from `IGpuResource` — destruction notification flows
 * through `IGpuResourceObserver`, mutation notification through
 * `IRenderStateObserver`. A class may implement both when both
 * signals are useful.
 *
 * Chain: IInterface -> IRenderState
 */
class IRenderState
    : public Interface<IRenderState, IInterface,
                       VELK_UID("65cc712d-e8e7-4fee-9d4f-272a926f00e8")>
{
public:
    /**
     * @brief Registers @p obs for change notifications.
     *
     * Idempotent: registering the same observer twice yields one
     * notification per change. Observers are responsible for calling
     * `remove_render_state_observer` before they are destroyed.
     */
    virtual void add_render_state_observer(IRenderStateObserver* obs) = 0;

    /**
     * @brief Removes a previously registered observer.
     *
     * Safe to call if the observer was never added.
     */
    virtual void remove_render_state_observer(IRenderStateObserver* obs) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_STATE_H
