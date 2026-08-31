#ifndef INTEROP_BRIDGE_H
#define INTEROP_BRIDGE_H

#include <QtCore/qsharedpointer.h>
#include <memory>

namespace qtng {
class SocketLike;
}

// Built in the qtng/qt private-bridge environment (see interop_bridge.cpp).
// The returned pointer actually points to a qtng_core::SocketLike (the core
// namespace used inside the qtnetworkng binary); it is type-erased because
// this header is consumed without the namespace remapping.
std::shared_ptr<void> interopToCoreSocketLikeErased(
        const QSharedPointer<qtng::SocketLike> &qt);

#endif
