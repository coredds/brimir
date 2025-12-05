#include <brimir/core/configuration.hpp>

namespace brimir::core {

void Configuration::NotifyObservers() {
    system.preferredRegionOrder.Notify();

    video.threadedVDP.Notify();

    audio.interpolation.Notify();
    audio.threadedSCSP.Notify();
}

} // namespace brimir::core
