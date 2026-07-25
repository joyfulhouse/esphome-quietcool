#pragma once

#include "quietcool/core/authority_store.h"

#include <optional>

namespace quietcool {

class AuthorityPublicationGate final {
 public:
  std::optional<ConfirmedStateAuthority> next(
      const AuthoritySnapshot& authority);

 private:
  std::optional<std::uint64_t> published_revision_;
};

}  // namespace quietcool
