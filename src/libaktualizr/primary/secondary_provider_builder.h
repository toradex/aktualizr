#ifndef UPTANE_SECONDARY_PROVIDER_BUILDER_H
#define UPTANE_SECONDARY_PROVIDER_BUILDER_H

#include <memory>

#include "libaktualizr/secondary_provider.h"

class HttpInterface;

class SecondaryProviderBuilder {
 public:
  static std::shared_ptr<SecondaryProvider> Build(
      Config& config, const std::shared_ptr<const INvStorage>& storage,
      const std::shared_ptr<const PackageManagerInterface>& package_manager,
      const std::shared_ptr<HttpInterface>& http = nullptr) {
    return std::make_shared<SecondaryProvider>(SecondaryProvider(config, storage, package_manager, http));
  }
  ~SecondaryProviderBuilder() = default;
  SecondaryProviderBuilder(const SecondaryProviderBuilder &) = delete;
  SecondaryProviderBuilder(SecondaryProviderBuilder &&) = delete;
  SecondaryProviderBuilder &operator=(const SecondaryProviderBuilder &) = delete;
  SecondaryProviderBuilder &operator=(SecondaryProviderBuilder &&) = delete;

 private:
  SecondaryProviderBuilder() = default;
};
#endif  // UPTANE_SECONDARY_PROVIDER_BUILDER_H
