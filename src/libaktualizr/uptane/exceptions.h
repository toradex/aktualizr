#ifndef UPTANE_EXCEPTIONS_H_
#define UPTANE_EXCEPTIONS_H_

#include <stdexcept>
#include <string>
#include <utility>
#include "libaktualizr/types.h"
#include "uptane/tuf.h"

namespace Uptane {

/* This is a list of different attributes that an Exception can optionally have.
 *
 * kPermanent: The exception requires intervention from the server to resolve itself.
 * Such exceptions should send a failure report to the server so that action can be taken.
 *
 * kTemporary: The exception is typically temporary in nature and might be resolved simply
 * by allowing the client to try again locally. Thus a failure report should not be sent
 * in such cases to allow additional attempts by the client.
 */
enum class Persistence { kPermanent = 0, kTemporary };

class Exception : public std::logic_error {
 public:
  Exception(RepositoryType repo_type, const std::string& what_arg, Persistence persistence = Persistence::kPermanent)
      : std::logic_error(what_arg.c_str()), subject_{repo_type.ToString()}, persistence_(persistence) {}
  // NOLINTNEXTLINE(modernize-pass-by-value) Would require changing caller's types too
  Exception(const std::string& subject, const std::string& what_arg, Persistence persistence = Persistence::kPermanent)
      : std::logic_error(what_arg.c_str()), subject_{subject}, persistence_(persistence) {}
  [[nodiscard]] virtual std::string getName() const { return subject_; };
  [[nodiscard]] Persistence getPersistence() const { return persistence_; };

 protected:
  std::string subject_;
  Persistence persistence_;
};

class MetadataFetchFailure : public Exception {
 public:
  MetadataFetchFailure(RepositoryType repo_type, const std::string& role,
                       const Persistence persistence = Persistence::kTemporary)
      : Exception(repo_type,
                  std::string("Failed to fetch role ") + role + " in " + repo_type.ToString() + " repository.",
                  persistence) {}
};

class SecurityException : public Exception {
 public:
  SecurityException(RepositoryType repo_type, const std::string& what_arg,
                    const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, what_arg, persistence) {}
};

class TargetContentMismatch : public Exception {
 public:
  explicit TargetContentMismatch(const std::string& targetname, const Persistence persistence = Persistence::kPermanent)
      : Exception(
            RepositoryType::Director(),
            "Director Target filename " + targetname + " matches currently installed version, but content differs.",
            persistence) {}
};

class TargetHashMismatch : public Exception {
 public:
  explicit TargetHashMismatch(const std::string& targetname, const Persistence persistence = Persistence::kPermanent)
      : Exception(targetname, "The target's calculated hash did not match the hash in the metadata.", persistence) {}
};

class OversizedTarget : public Exception {
 public:
  explicit OversizedTarget(const std::string& reponame, const Persistence persistence = Persistence::kPermanent)
      : Exception(reponame, "The target's size was greater than the size in the metadata.", persistence) {}
};

class IllegalThreshold : public Exception {
 public:
  IllegalThreshold(RepositoryType repo_type, const std::string& what_arg,
                   const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, what_arg, persistence) {}
};

class UnmetThreshold : public Exception {
 public:
  UnmetThreshold(RepositoryType repo_type, const std::string& role,
                 const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "The " + role + " metadata had an unmet threshold.", persistence) {}
};

class ExpiredMetadata : public Exception {
 public:
  ExpiredMetadata(RepositoryType repo_type, const std::string& role,
                  const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "The " + role + " metadata was expired.", persistence) {}
};

class InvalidMetadata : public Exception {
 public:
  InvalidMetadata(RepositoryType repo_type, const std::string& role, const std::string& reason,
                  const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "The " + role + " metadata failed to parse: " + reason, persistence) {}
  explicit InvalidMetadata(const std::string& reason, const Persistence persistence = Persistence::kPermanent)
      : Exception("", "The metadata failed to parse: " + reason, persistence) {}
};

class TargetMismatch : public Exception {
 public:
  explicit TargetMismatch(const std::string& targetname, const Persistence persistence = Persistence::kPermanent)
      : Exception(targetname, "The target metadata in the Image and Director repos do not match.", persistence) {}
};

class NonUniqueSignatures : public Exception {
 public:
  NonUniqueSignatures(RepositoryType repo_type, const std::string& role,
                      const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "The role " + role + " had non-unique signatures.", persistence) {}
};

class BadKeyId : public Exception {
 public:
  explicit BadKeyId(RepositoryType repo_type, const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "A key has an incorrect associated key ID", persistence) {}
};

class BadEcuId : public Exception {
 public:
  explicit BadEcuId(const std::string& reponame, const Persistence persistence = Persistence::kPermanent)
      : Exception(reponame, "The target had an ECU ID that did not match the client's configured ECU ID.",
                  persistence) {}
};

class BadHardwareId : public Exception {
 public:
  explicit BadHardwareId(const std::string& reponame, const Persistence persistence = Persistence::kPermanent)
      : Exception(reponame, "The target had a hardware ID that did not match the client's configured hardware ID.",
                  persistence) {}
};

class RootRotationError : public Exception {
 public:
  explicit RootRotationError(RepositoryType repo_type, const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "Version in Root metadata does not match its expected value.", persistence) {}
};

class VersionMismatch : public Exception {
 public:
  VersionMismatch(RepositoryType repo_type, const std::string& role,
                  const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "The version of role " + role + " does not match the entry in Snapshot metadata.",
                  persistence) {}
};

class DelegationHashMismatch : public Exception {
 public:
  explicit DelegationHashMismatch(const std::string& delegation_name,
                                  const Persistence persistence = Persistence::kPermanent)
      : Exception(
            "image",
            "The calculated hash of delegated role " + delegation_name + " did not match the hash in the metadata.",
            persistence) {}
};

class DelegationMissing : public Exception {
 public:
  explicit DelegationMissing(const std::string& delegation_name,
                             const Persistence persistence = Persistence::kPermanent)
      : Exception("image", "The delegated role " + delegation_name + " is missing.", persistence) {}
};

class InvalidTarget : public Exception {
 public:
  explicit InvalidTarget(const std::string& reponame, const Persistence persistence = Persistence::kPermanent)
      : Exception(reponame, "The target had a non-OSTree package that can not be installed on an OSTree system.",
                  persistence) {}
};

class LocallyAborted : public Exception {
 public:
  explicit LocallyAborted(RepositoryType repo_type, const Persistence persistence = Persistence::kPermanent)
      : Exception(repo_type, "Update was aborted on the client", persistence) {}
};

}  // namespace Uptane

#endif
