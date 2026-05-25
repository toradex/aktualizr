#ifndef PRIMARY_SYSTEMD_JOURNAL_H_
#define PRIMARY_SYSTEMD_JOURNAL_H_

#include <cstdint>
#include <memory>
#include <string>

#include "primary/journal.h"

// Forward declaration - full definition in <systemd/sd-journal.h>
struct sd_journal;

/**
 * RAII wrapper for sd_journal*. Opens the journal in the constructor;
 * use IsOpen()/operator bool() to check if the open succeeded.
 *
 * This is the production implementation of the JournalHandle interface; it
 * wraps libsystemd's sd-journal API. Tests substitute TestJournal instead.
 */
class SystemdJournal : public JournalHandle {
 public:
  SystemdJournal();
  ~SystemdJournal() override;

  SystemdJournal(const SystemdJournal&) = delete;
  SystemdJournal& operator=(const SystemdJournal&) = delete;
  SystemdJournal(SystemdJournal&&) = delete;
  SystemdJournal& operator=(SystemdJournal&&) = delete;

  std::unique_ptr<JournalHandle> Clone() const override { return std::make_unique<SystemdJournal>(); }

  bool IsOpen() const override { return journal_ != nullptr; }

  int Next() override;
  std::string GetField(const char* field) override;
  int64_t GetTimestamp() override;
  bool MoveTail() override;
  std::string GetCurrentCursor() override;

 private:
  sd_journal* journal_{nullptr};
};

#endif  // PRIMARY_SYSTEMD_JOURNAL_H_
