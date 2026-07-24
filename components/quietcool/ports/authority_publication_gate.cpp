#include "authority_publication_gate.h"

namespace quietcool {

std::optional<ConfirmedStateAuthority> AuthorityPublicationGate::next(
    const AuthoritySnapshot& authority) {
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&authority.state);
  if (confirmed == nullptr ||
      (published_revision_ && confirmed->revision <= *published_revision_))
    return std::nullopt;
  published_revision_ = confirmed->revision;
  return *confirmed;
}

}  // namespace quietcool
