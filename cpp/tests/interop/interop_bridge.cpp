// Build bridge: expose a Qt-binding -> std-core SocketLike converter to the
// interop test. This TU is compiled with the `qtng=qtng_core` remapping (same
// as the qtng/qt binding's own bridge TUs), so it can call
// qtng_bridge::toCoreSocketLike and re-export it without macro pollution.
// The returned pointer is type-erased: the rpc side sees it as
// qtng_core::SocketLike, the test side as an opaque shared_ptr.
#include "bridge/stream_bridge.h"

std::shared_ptr<void> interopToCoreSocketLikeErased(
        const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &qt)
{
    return std::shared_ptr<void>(qtng_bridge::toCoreSocketLike(qt));
}
