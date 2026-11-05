//
// RT64
//

#include "rt64_texture_names.h"

#include <atomic>

namespace RT64 {
    static std::atomic<const TextureNameProvider *> installedProvider{ nullptr };

    void setTextureNameProvider(const TextureNameProvider *provider) {
        installedProvider.store(provider);
    }

    const TextureNameProvider *textureNameProvider() {
        return installedProvider.load();
    }
};
