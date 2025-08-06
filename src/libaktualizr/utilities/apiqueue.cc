#include "apiqueue.h"
#include "logging/logging.h"

namespace api {

CommandQueue::~CommandQueue() {
  try {
    {
      std::lock_guard<std::mutex> lock(m_);
      token_.setAbort();
      shutdown_ = true;
    }
    cv_.notify_all();  // Wake up the run thread
    if (thread_.joinable()) {
      thread_.join();
    }
  } catch (std::exception& ex) {
    LOG_ERROR << "~CommandQueue() exception: " << ex.what() << std::endl;
  } catch (...) {
    LOG_ERROR << "~CommandQueue() unknown exception" << std::endl;
  }
  // The dtors for the queue_ entries will complete the enqueued tasks
}

void CommandQueue::run() {
  std::lock_guard<std::mutex> lock(m_);
  if (thread_.joinable()) {
    // Already running
    return;
  }
  // Start the BG thread
  thread_ = std::thread([this] {
    Context ctx{&token_};
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
      cv_.wait(lock, [this] { return (!queue_.empty() && !paused_) || shutdown_ || cancelling_; });
      if (shutdown_) {
        break;
      } else if (cancelling_) {
        // Drop the jobs on the floor. The dtor will take care of setting the
        // future correctly
        cancelled_jobs_.clear();
        // Reset the flow_control_token. This will also un-pause the queue
        token_.reset();
        // Notify the callers of CommandQueue::Cancel() that their cancellation
        // is complete
        cancel_done_.set_value();
        cancelling_ = false;
      } else {
        assert(!queue_.empty());
        auto task = std::move(queue_.front());
        queue_.pop_front();
        token_.reset();
        lock.unlock();
        task->PerformTask(&ctx);
        lock.lock();
      }
    }
  });
}

bool CommandQueue::pause(bool do_pause) {
  bool has_effect;
  {
    std::lock_guard<std::mutex> lock(m_);
    has_effect = paused_ != do_pause;
    paused_ = do_pause;
    token_.setPause(do_pause);
  }
  cv_.notify_all();

  return has_effect;
}

std::shared_future<void> CommandQueue::Cancel() {
  std::lock_guard<std::mutex> g(m_);
  if (cancellation_in_progress_.valid()) {
    // There is already a cancellation in progress
    return cancellation_in_progress_;
  }

  // Cancellation process:
  //  - Move the queued-but-not-started jobs to a 'cancelled' queue
  //  - Abort the current running job
  //  - Create a promise for the run thread to report back on
  //  - Wake the runn thread up via cv_
  //  - Return the promise
  // The run thread will then wake up and:
  //  - Wait for the current op to finish
  //  - Resolve the cancelled jobs with the value passed to enqueue()
  //  - resolve the promise attached to the future we returned

  cancelled_jobs_.splice(cancelled_jobs_.end(), queue_);
  token_.setAbort();

  std::promise<void> new_promise;
  std::swap(new_promise, cancel_done_);
  cancellation_in_progress_ = new_promise.get_future().share();
  cancelling_ = true;
  cv_.notify_all();
  return cancellation_in_progress_;
}

void CommandQueue::enqueue(ICommand::Ptr&& task) {
  {
    std::lock_guard<std::mutex> lock(m_);
    queue_.push_back(std::move(task));
  }
  cv_.notify_all();
}

}  // namespace api
