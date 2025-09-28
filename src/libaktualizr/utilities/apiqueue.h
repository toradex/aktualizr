#ifndef AKTUALIZR_APIQUEUE_H
#define AKTUALIZR_APIQUEUE_H

#include <condition_variable>
#include <functional>
#include <future>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

#include "utilities/flow_control.h"

namespace api {

struct Context {
  api::FlowControlToken* flow_control;
};

class ICommand {
 public:
  using Ptr = std::shared_ptr<ICommand>;
  ICommand() = default;
  virtual ~ICommand() = default;
  // Non-movable-non-copyable
  ICommand(const ICommand&) = delete;
  ICommand(ICommand&&) = delete;
  ICommand& operator=(const ICommand&) = delete;
  ICommand& operator=(ICommand&&) = delete;

  virtual void PerformTask(Context* ctx) = 0;
};

/** A command that doesn't do flow control, but returns a result */
template <class T>
class CommandResult : public ICommand {
 public:
  // TODO: Investigate this
  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  explicit CommandResult(std::function<T()>&& f, T&& result_on_cancellation)
      : f_{f}, result_on_cancellation_(result_on_cancellation) {}
  ~CommandResult() override {
    if (!has_finished_) {
      result_.set_value(std::move(result_on_cancellation_));
    }
  }

  CommandResult(const CommandResult&) = delete;
  CommandResult(CommandResult&&) = delete;
  CommandResult& operator=(const CommandResult&) = delete;
  CommandResult& operator=(CommandResult&&) = delete;

  void PerformTask(Context* ctx) override {
    (void)ctx;
    try {
      result_.set_value(f_());
    } catch (...) {
      result_.set_exception(std::current_exception());
    }
    has_finished_ = true;
  }

  std::future<T> GetFuture() { return result_.get_future(); }

 private:
  std::function<T()> f_;
  std::promise<T> result_;
  bool has_finished_{false};
  T result_on_cancellation_;
};

/** A command that doesn't do flow control and returns void */
class CommandVoid : public ICommand {
 public:
  // TODO: Investigate this
  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  explicit CommandVoid(std::function<void()>&& f) : f_{f} {}
  ~CommandVoid() override {
    if (!has_finished_) {
      result_.set_value();
    }
  }

  CommandVoid(const CommandVoid&) = delete;
  CommandVoid(CommandVoid&&) = delete;
  CommandVoid& operator=(const CommandVoid&) = delete;
  CommandVoid& operator=(CommandVoid&&) = delete;

  void PerformTask(Context* ctx) override {
    (void)ctx;
    try {
      f_();
      result_.set_value();
    } catch (...) {
      result_.set_exception(std::current_exception());
    }
    has_finished_ = true;
  }

  std::future<void> GetFuture() { return result_.get_future(); }

 private:
  std::function<void()> f_;
  std::promise<void> result_;
  bool has_finished_{false};
};

template <class T>
class CommandFlowControl : public ICommand {
 public:
  // TODO: Investigate this
  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  explicit CommandFlowControl(std::function<T(const api::FlowControlToken*)>&& func, T&& result_on_cancellation)
      : f_{func}, result_on_cancellation_(result_on_cancellation) {}

  ~CommandFlowControl() override {
    if (!has_finished_) {
      result_.set_value(std::move(result_on_cancellation_));
    }
  }

  CommandFlowControl(const CommandFlowControl&) = delete;
  CommandFlowControl(CommandFlowControl&&) = delete;
  CommandFlowControl& operator=(const CommandFlowControl&) = delete;
  CommandFlowControl& operator=(CommandFlowControl&&) = delete;

  void PerformTask(Context* ctx) override {
    try {
      result_.set_value(f_(ctx->flow_control));
    } catch (...) {
      result_.set_exception(std::current_exception());
    }
    has_finished_ = true;
  }

  std::future<T> GetFuture() { return result_.get_future(); }

 private:
  std::function<T(const api::FlowControlToken*)> f_;
  std::promise<T> result_;
  bool has_finished_{false};
  T result_on_cancellation_;
};

class CommandQueue {
 public:
  CommandQueue() = default;
  ~CommandQueue();
  // Non-copyable Non-movable
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue(CommandQueue&&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;
  CommandQueue& operator=(CommandQueue&&) = delete;
  void run();
  bool pause(bool do_pause);  // returns true iff pause→resume or resume→pause

  /**
   * Stop any current operation and flush everything after it in the queue.
   * The item at the head of the queue will be cancelled by aborting the
   * FlowControlToken. It is up to the individual command to stop at an
   * appropriate point so if an installation is in flight, this may take several
   * seconds. Items in the tail of the queue will resolve to the result_on_cancellation
   * that was passed in to enqueue().
   * Multiple cancellations in parallel are allowed.
   * Enqueing a task directly after calling Cancel() is OK. This new task will
   * be the first to execute after the Cancellation is complete.
   */
  std::shared_future<void> Cancel();

  const api::FlowControlToken* FlowControlToken() const { return &token_; }

  template <class R>
  std::future<R> enqueue(std::function<R()>&& function, R&& result_on_cancellation) {
    auto task = std::make_shared<CommandResult<R>>(std::move(function), std::forward<R>(result_on_cancellation));
    enqueue(task);
    return task->GetFuture();
  }

  std::future<void> enqueue(std::function<void()>&& function) {
    auto task = std::make_shared<CommandVoid>(std::move(function));
    enqueue(task);
    return task->GetFuture();
  }

  template <class R>
  std::future<R> enqueue(std::function<R(const api::FlowControlToken*)>&& function, R&& result_on_cancellation) {
    auto task = std::make_shared<CommandFlowControl<R>>(std::move(function), std::forward<R>(result_on_cancellation));
    enqueue(task);
    return task->GetFuture();
  }

  // A void-returning FlowControlToken accepting function would be possible,
  // but is not needed at the moment
  // std::future<void> enqueue(std::function<void(const api::FlowControlToken*)>&& function);

 private:
  void enqueue(ICommand::Ptr&& task);
  std::mutex m_;
  std::condition_variable cv_;
  std::thread thread_;
  bool shutdown_{false};
  bool paused_{false};
  bool cancelling_{false};
  std::promise<void> cancel_done_;
  std::shared_future<void> cancellation_in_progress_;

  /** All the enqueued but not started commands */
  std::list<ICommand::Ptr> queue_;

  /**
   * These jobs are logically ahead of those in queue_, but have been cancelled via
   * CommandQueue::Cancel(). The associated futures will be resolved by the
   * run() thread, after any in-flight job is complete. This ensures that
   * jobs complete in the order they are enqueued.
   */
  std::list<ICommand::Ptr> cancelled_jobs_;
  class api::FlowControlToken token_;
};

}  // namespace api
#endif  // AKTUALIZR_APIQUEUE_H
