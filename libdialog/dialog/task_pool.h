#ifndef DIALOG_TASK_QUEUE_H_
#define DIALOG_TASK_QUEUE_H_

#include <queue>
#include <mutex>
#include <future>
#include <functional>

#include "logger.h"

// TODO: Can potentially make this more efficient with lock-free concurrency
// Although seems unnecessary for now
struct task_type {
  std::function<void()> func;
  task_type* next;

  template<class ... ARGS>
  task_type(ARGS&&... args)
      : func(std::forward<ARGS>(args)...),
        next(nullptr) {
  }
};

class task_queue {
 public:
  typedef std::function<void()> function_t;

  task_queue()
      : valid_(true) {
  }

  ~task_queue() {
    invalidate();
  }

  void invalidate(void) {
    std::lock_guard<std::mutex> lock { mutex_ };
    valid_ = false;
    condition_.notify_all();
  }

  /**
   * Get the first value in the queue.
   * Will block until a value is available unless clear is called or the instance is destructed.
   * Returns true if a value was successfully written to the out parameter, false otherwise.
   */
  bool dequeue(function_t& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() {return !queue_.empty() || !valid_;});

    /*
     * Using the condition in the predicate ensures that spurious wakeups with a valid
     * but empty queue will not proceed, so only need to check for validity before proceeding.
     */
    if (!valid_)
      return false;

    out = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  /**
   * Push a new value onto the queue.
   */
  template<class F, class ...ARGS>
  auto enqueue(F&& f, ARGS&&... args)
  -> std::future<typename std::result_of<F(ARGS...)>::type> {
    using return_type = typename std::result_of<F(ARGS...)>::type;

    auto task = std::make_shared<std::packaged_task<return_type()> >(
        std::bind(std::forward<F>(f), std::forward<ARGS>(args)...));
    std::future<return_type> res = task->get_future();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.emplace([task]() {(*task)();});
      condition_.notify_one();
    }

    return res;
  }

  /**
   * Check whether or not the queue is empty.
   */
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  /**
   * Clear all items from the queue.
   */
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      queue_.pop();
    }
    condition_.notify_all();
  }

  /**
   * Returns whether or not this queue is valid.
   */
  bool is_valid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_;
  }

 private:
  atomic::type<bool> valid_;
  std::atomic_bool valid { true };
  mutable std::mutex mutex_;
  std::queue<function_t> queue_;
  std::condition_variable condition_;
};

class task_worker {
 public:
  task_worker(task_queue& queue)
      : stop_(false),
        queue_(queue) {
  }

  ~task_worker() {
    stop();
  }

  void start() {
    worker_ = std::thread([this]() {
      task_queue::function_t task;
      while (!atomic::load(&stop_)) {
        if (queue_.dequeue(task)) {
          try {
            task();
          } catch(std::exception& e) {
            LOG_ERROR << "Could not execute task: " << e.what();
            fprintf(stderr, "Exception: %s\n", e.what());
          }
        }
      }
    });
  }

  void stop() {
    atomic::store(&stop_, true);
    if (worker_.joinable())
      worker_.join();
  }

 private:
  atomic::type<bool> stop_;
  task_queue& queue_;
  std::thread worker_;
};

class task_pool {
 public:
  task_pool(size_t num_workers = 1) {
    for (size_t i = 0; i < num_workers; i++) {
      workers_.push_back(new task_worker(queue_));
      workers_[i]->start();
    }
  }

  ~task_pool() {
    queue_.invalidate();
    for (task_worker* worker : workers_) {
      delete worker;
    }
  }

  template<class F, class ...ARGS>
  auto submit(F&& f, ARGS&&... args)
  -> std::future<typename std::result_of<F(ARGS...)>::type> {
    return queue_.enqueue(std::forward<F>(f), std::forward<ARGS>(args)...);
  }

 private:
  task_queue queue_;
  std::vector<task_worker*> workers_;
};

#endif /* DIALOG_TASK_QUEUE_H_ */
