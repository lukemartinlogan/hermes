/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef HRUN_INCLUDE_HRUN_WORK_ORCHESTRATOR_WORKER_H_
#define HRUN_INCLUDE_HRUN_WORK_ORCHESTRATOR_WORKER_H_

#include "hrun/hrun_types.h"
#include "hrun/queue_manager/queue_manager_runtime.h"
#include "hrun/task_registry/task_registry.h"
#include "hrun/work_orchestrator/work_orchestrator.h"
#include "hrun/api/hrun_runtime_.h"
#include <thread>
#include <queue>
#include "affinity.h"
#include "hrun/network/rpc_thallium.h"

#define HSHM_WORKER_MAX_TASK_DEPTH HSHM_MAX_QUEUE_GROUP_DEPTH

static inline pid_t GetLinuxTid() {
  return syscall(SYS_gettid);
}

#define HSHM_WORKER_IS_REMOTE BIT_OPT(u32, 0)
#define HSHM_WORKER_GROUP_AVAIL BIT_OPT(u32, 1)
#define HSHM_WORKER_SHOULD_RUN BIT_OPT(u32, 2)
#define HSHM_WORKER_IS_FLUSHING BIT_OPT(u32, 3)
#define HSHM_WORKER_LONG_RUNNING BIT_OPT(u32, 4)

namespace hrun {

#define WORKER_CONTINUOUS_POLLING BIT_OPT(u32, 0)

/** Uniquely identify a queue lane */
struct WorkEntry {
  u32 prio_;
  u32 lane_id_;
  std::array<Lane*, HSHM_MAX_QUEUE_GROUP_DEPTH> lanes_;
  LaneGroup *group_;
  MultiQueue *queue_;

  /** Default constructor */
  HSHM_ALWAYS_INLINE
  WorkEntry() = default;

  /** Emplace constructor */
  HSHM_ALWAYS_INLINE
  WorkEntry(u32 prio, u32 lane_id, MultiQueue *queue)
  : prio_(prio), lane_id_(lane_id), queue_(queue) {
    group_ = &queue->GetGroup(prio);
    for (int depth = 0; depth < HSHM_MAX_QUEUE_GROUP_DEPTH; ++depth) {
      lanes_[depth] = &queue->GetLane(prio, lane_id, depth);
    }
  }

  /** Copy constructor */
  HSHM_ALWAYS_INLINE
  WorkEntry(const WorkEntry &other) {
    prio_ = other.prio_;
    lane_id_ = other.lane_id_;
    lanes_ = other.lanes_;
    group_ = other.group_;
    queue_ = other.queue_;
  }

  /** Copy assignment */
  HSHM_ALWAYS_INLINE
  WorkEntry& operator=(const WorkEntry &other) {
    if (this != &other) {
      prio_ = other.prio_;
      lane_id_ = other.lane_id_;
      lanes_ = other.lanes_;
      group_ = other.group_;
      queue_ = other.queue_;
    }
    return *this;
  }

  /** Move constructor */
  HSHM_ALWAYS_INLINE
  WorkEntry(WorkEntry &&other) noexcept {
    prio_ = other.prio_;
    lane_id_ = other.lane_id_;
    lanes_ = other.lanes_;
    group_ = other.group_;
    queue_ = other.queue_;
  }

  /** Move assignment */
  HSHM_ALWAYS_INLINE
  WorkEntry& operator=(WorkEntry &&other) noexcept {
    if (this != &other) {
      prio_ = other.prio_;
      lane_id_ = other.lane_id_;
      lanes_ = other.lanes_;
      group_ = other.group_;
      queue_ = other.queue_;
    }
    return *this;
  }

  /** Check if null */
  [[nodiscard]]
  HSHM_ALWAYS_INLINE bool IsNull() const {
    return queue_->IsNull();
  }

  /** Equality operator */
  HSHM_ALWAYS_INLINE
  bool operator==(const WorkEntry &other) const {
    return queue_ == other.queue_ && lane_id_ == other.lane_id_ &&
        prio_ == other.prio_;
  }
};

}  // namespace hrun

namespace std {
/** Hash function for WorkEntry */
template<>
struct hash<hrun::WorkEntry> {
  HSHM_ALWAYS_INLINE
      std::size_t
  operator()(const hrun::WorkEntry &key) const {
    return std::hash<hrun::MultiQueue*>{}(key.queue_) +
        std::hash<u32>{}(key.lane_id_) + std::hash<u64>{}(key.prio_);
  }
};
}  // namespace std

namespace hrun {

struct PrivateTaskQueueEntry {
 public:
  LPointer<Task> task_;
  WorkEntry *lane_info_;

 public:
  PrivateTaskQueueEntry() = default;
  PrivateTaskQueueEntry(const LPointer<Task> &task, WorkEntry *lane_info)
  : task_(task), lane_info_(lane_info) {}

  PrivateTaskQueueEntry(const PrivateTaskQueueEntry &other) {
    task_ = other.task_;
    lane_info_ = other.lane_info_;
  }

  PrivateTaskQueueEntry& operator=(const PrivateTaskQueueEntry &other) {
    if (this != &other) {
      task_ = other.task_;
      lane_info_ = other.lane_info_;
    }
    return *this;
  }

  PrivateTaskQueueEntry(PrivateTaskQueueEntry &&other) noexcept {
    task_ = other.task_;
    lane_info_ = other.lane_info_;
  }

  PrivateTaskQueueEntry& operator=(PrivateTaskQueueEntry &&other) noexcept {
    if (this != &other) {
      task_ = other.task_;
      lane_info_ = other.lane_info_;
    }
    return *this;
  }
};

class PrivateTaskQueue {
 public:
  std::vector<PrivateTaskQueueEntry> queue_;
  size_t size_, head_, tail_;

 public:
  PrivateTaskQueue(size_t queue_depth) {
    queue_.resize(queue_depth);
    size_ = 0;
    tail_ = 0;
    head_ = 0;
  }

  bool push(const PrivateTaskQueueEntry &entry) {
    size_t diff = tail_ - head_;
    if (diff >= queue_.size()) {
      return false;
    }
    queue_[tail_ % queue_.size()] = entry;
    ++size_;
    ++tail_;
    return true;
  }

  void peek(size_t off, PrivateTaskQueueEntry &entry) {
    entry = queue_[off % queue_.size()];
  }

  void erase(size_t off) {
    queue_[off % queue_.size()].task_.ptr_ = nullptr;
    --size_;
    _correct_head();
  }

  void _correct_head() {
    for (size_t i = head_; head_ < tail_; ++i) {
      if (queue_[i % queue_.size()].task_.ptr_ == nullptr) {
        head_ = i;
      } else {
        break;
      }
    }
  }
};

class PrivateTaskMultiQueue {
 public:
  std::vector<PrivateTaskQueue> low_lat_;
  std::vector<PrivateTaskQueue> high_lat_;
  std::vector<PrivateTaskQueue> long_running_;

 public:
  void Init(int max_task_depth, size_t queue_depth) {
    low_lat_.resize(max_task_depth, queue_depth);
    high_lat_.resize(max_task_depth, queue_depth);
    long_running_.resize(1, queue_depth);
  }

  PrivateTaskQueue& GetLowLatency(int task_depth) {
    if (task_depth >= low_lat_.size()) {
      task_depth = low_lat_.size() - 1;
    }
    return low_lat_[task_depth];
  }

  PrivateTaskQueue& GetHighLatency(int task_depth) {
    if (task_depth >= high_lat_.size()) {
      task_depth = high_lat_.size() - 1;
    }
    return high_lat_[task_depth];
  }

  PrivateTaskQueue& GetLongRunning() {
    return long_running_[0];
  }
};

class Worker {
 public:
  u32 id_;  /**< Unique identifier of this worker */
  std::unique_ptr<std::thread> thread_;  /**< The worker thread handle */
  ABT_thread tl_thread_;  /**< The worker argobots thread handle */
  int pthread_id_;      /**< The worker pthread handle */
  std::atomic<int> pid_;  /**< The worker process id */
  int affinity_;        /**< The worker CPU affinity */
  u32 numa_node_;       // TODO(llogan): track NUMA affinity
  ABT_xstream xstream_;
  std::vector<WorkEntry> work_queue_;  /**< The set of queues to poll */
  /**< A set of queues to begin polling in a worker */
  hshm::spsc_queue<std::vector<WorkEntry>> poll_queues_;
  /**< A set of queues to stop polling in a worker */
  hshm::spsc_queue<std::vector<WorkEntry>> relinquish_queues_;
  size_t sleep_us_;     /**< Time the worker should sleep after a run */
  u32 retries_;         /**< The number of times to repeat the internal run loop before sleeping */
  bitfield32_t flags_;  /**< Worker metadata flags */
  std::unordered_map<hshm::charbuf, TaskNode>
      group_map_;        /**< Determine if a task can be executed right now */
  std::unordered_map<TaskStateId, TaskState*>
      state_map_;       /**< The set of task states */
  hshm::charbuf group_;  /**< The current group */
  WorkPending flush_;    /**< Info needed for flushing ops */
  hshm::Timepoint now_;  /**< The current timepoint */
  hshm::spsc_queue<void*> stacks_;  /**< Cache of stacks for tasks */
  int num_stacks_ = 256;  /**< Number of stacks */
  int stack_size_ = KILOBYTES(64);
  PrivateTaskMultiQueue
      pending_;  /** Tasks pending to complete */
  std::list<PrivateTaskQueueEntry>
      long_running_;  /** Long running tasks pending to complete */
  hshm::Timepoint cur_time_;  /**< The current timepoint */
  size_t work_;

 public:
  /**===============================================================
   * Initialize Worker and Change Utilization
   * =============================================================== */

  /** Constructor */
  Worker(u32 id, int cpu_id, ABT_xstream &xstream) {
    poll_queues_.Resize(1024);
    relinquish_queues_.Resize(1024);
    id_ = id;
    sleep_us_ = 0;
    retries_ = 1;
    pid_ = 0;
    affinity_ = cpu_id;
    // TODO(llogan): implement reserve for group
    group_.resize(512);
    group_.resize(0);
    stacks_.Resize(num_stacks_);
    for (int i = 0; i < 16; ++i) {
      stacks_.emplace(malloc(stack_size_));
    }
    // MAX_DEPTH * [LOW_LAT, LONG_LAT]
    config::QueueManagerInfo &qm = HRUN_QM_RUNTIME->config_->queue_manager_;
    pending_.Init(HSHM_WORKER_MAX_TASK_DEPTH,
                  qm.queue_depth_);
    cur_time_.Now();

    // Spawn threads
    xstream_ = xstream;
    thread_ = std::make_unique<std::thread>(&Worker::Loop, this);
    pthread_id_ = thread_->native_handle();
  }

  /** Constructor without threading */
  Worker(u32 id) {
    poll_queues_.Resize(1024);
    relinquish_queues_.Resize(1024);
    id_ = id;
    sleep_us_ = 0;
    EnableContinuousPolling();
    retries_ = 1;
    pid_ = 0;
    pthread_id_ = GetLinuxTid();
    // TODO(llogan): implement reserve for group
    group_.resize(512);
    group_.resize(0);
  }

  /** Tell worker to poll a set of queues */
  void PollQueues(const std::vector<WorkEntry> &queues) {
    poll_queues_.emplace(queues);
  }

  /** Actually poll the queues from within the worker */
  void _PollQueues() {
    std::vector<WorkEntry> work_queue;
    while (!poll_queues_.pop(work_queue).IsNull()) {
      for (const WorkEntry &entry : work_queue) {
        // HILOG(kDebug, "Scheduled queue {} (lane {})", entry.queue_->id_, entry.lane_);
        work_queue_.emplace_back(entry);
      }
    }
    HILOG(kInfo, "Worker {} has {} lanes", id_, work_queue_.size())
  }

  /**
   * Tell worker to start relinquishing some of its queues
   * This function must be called from a single thread (outside of worker)
   * */
  void RelinquishingQueues(const std::vector<WorkEntry> &queues) {
    relinquish_queues_.emplace(queues);
  }

  /** Actually relinquish the queues from within the worker */
  void _RelinquishQueues() {
    std::vector<WorkEntry> work_queue;
    while (!poll_queues_.pop(work_queue).IsNull()) {
      for (auto &entry : work_queue) {
        work_queue_.erase(std::find(work_queue_.begin(),
                                    work_queue_.end(), entry));
      }
    }
  }

  /** Check if worker is still stealing queues */
  bool IsRelinquishingQueues() {
    return relinquish_queues_.size() > 0;
  }

  /** Set the sleep cycle */
  void SetPollingFrequency(size_t sleep_us, u32 num_retries) {
    sleep_us_ = sleep_us;
    retries_ = num_retries;
    flags_.UnsetBits(WORKER_CONTINUOUS_POLLING);
  }

  /** Enable continuous polling */
  void EnableContinuousPolling() {
    flags_.SetBits(WORKER_CONTINUOUS_POLLING);
  }

  /** Disable continuous polling */
  void DisableContinuousPolling() {
    flags_.UnsetBits(WORKER_CONTINUOUS_POLLING);
  }

  /** Check if continuously polling */
  bool IsContinuousPolling() {
    return flags_.Any(WORKER_CONTINUOUS_POLLING);
  }

  /** Set the CPU affinity of this worker */
  void SetCpuAffinity(int cpu_id) {
    HILOG(kInfo, "Affining worker {} (pid={}) to {}", id_, pid_, cpu_id);
    affinity_ = cpu_id;
    ProcessAffiner::SetCpuAffinity(pid_, affinity_);
  }

  /** Make maximum priority process */
  void MakeDedicated() {
    int policy = SCHED_FIFO;
    struct sched_param param = { .sched_priority = 1 };
    sched_setscheduler(0, policy, &param);
  }

  /** Worker yields for a period of time */
  void Yield() {
    if (flags_.Any(WORKER_CONTINUOUS_POLLING)) {
      return;
    }
    if (sleep_us_ > 0) {
      HERMES_THREAD_MODEL->SleepForUs(sleep_us_);
    } else {
      HERMES_THREAD_MODEL->Yield();
    }
  }

  /** Allocate a stack for a task */
  void* AllocateStack() {
    void *stack;
    if (!stacks_.pop(stack).IsNull()) {
      return stack;
    }
    return malloc(stack_size_);
  }

  /** Free a stack */
  void FreeStack(void *stack) {
    if(!stacks_.emplace(stack).IsNull()) {
      return;
    }
    stacks_.Resize(stacks_.size() + num_stacks_);
  }

  /**===============================================================
   * Run tasks
   * =============================================================== */

  /** Worker loop iteration */
  void Loop() {
    pid_ = GetLinuxTid();
    SetCpuAffinity(affinity_);
    if (IsContinuousPolling()) {
      MakeDedicated();
    }
    WorkOrchestrator *orchestrator = HRUN_WORK_ORCHESTRATOR;
    now_.Now();
    while (orchestrator->IsAlive()) {
      try {
        bool flushing = flush_.flushing_;
        Run(flushing);
        if (flushing) {
          flush_.flushing_ = false;
        }
      } catch (hshm::Error &e) {
        HELOG(kError, "(node {}) Worker {} caught an error: {}", HRUN_CLIENT->node_id_, id_, e.what());
      } catch (std::exception &e) {
        HELOG(kError, "(node {}) Worker {} caught an exception: {}", HRUN_CLIENT->node_id_, id_, e.what());
      } catch (...) {
        HELOG(kError, "(node {}) Worker {} caught an unknown exception", HRUN_CLIENT->node_id_, id_);
      }
      if (!IsContinuousPolling()) {
        Yield();
      }
    }
    Run(true);
  }

  /** Run a single iteration over all queues */
  void Run(bool flushing) {
    work_ = 0;
    // Are there any queues pending scheduling
    if (poll_queues_.size() > 0) {
      _PollQueues();
    }
    // Are there any queues pending descheduling
    if (relinquish_queues_.size() > 0) {
      _RelinquishQueues();
    }
    // Ingest tasks from the ingress queues
    IngestLanes(0, flushing);
    for (size_t i = 0; i < 10; ++i) {
      if (!IngestLanes(1, flushing)) {
        break;
      }
    }
    // Process tasks in the pending queues
    PollPending(flushing);
  }

  /** Ingest all lanes */
  HSHM_ALWAYS_INLINE
  size_t IngestLanes(size_t min_depth, bool flushing) {
    // Get tasks from every depth
    size_t work = 0;
    for (WorkEntry &work_entry : work_queue_) {
      work += IngestLane(work_entry, min_depth, flushing);
    }
    return work;

    // Keep processing tasks with depth > 1
//    for (size_t i = 0; i < 4; ++i) {
//      size_t work = 0;
//      for (WorkEntry &work_entry : work_queue_) {
//        work += IngestLane(work_entry, 1, flushing);
//      }
//      if (work) {
//        break;
//      }
//    }
  }

  /** Ingest a lane */
  HSHM_ALWAYS_INLINE
  size_t IngestLane(WorkEntry &lane_info, int min_depth, bool flushing) {
    // Ingest tasks from the ingress queues
    size_t pending = 0;
    for (int depth = min_depth; depth < HSHM_MAX_QUEUE_GROUP_DEPTH; ++depth) {
      Lane *&lane = lane_info.lanes_[depth];
      LaneData entry;
      while (true) {
        if (!lane->pop(entry).IsNull()) {
          LPointer<Task> task;
          task.shm_ = entry.p_;
          task.ptr_ = HRUN_CLIENT->GetMainPointer<Task>(entry.p_);
          TaskState *exec =
              RunTask(lane_info, task, lane_info.lane_id_, flushing);
          if (!task->IsModuleComplete()) {
            if (task->IsLongRunning()) {
              pending_.GetLongRunning().push(
                  PrivateTaskQueueEntry{task, &lane_info});
            } else if (task->prio_ == TaskPrio::kLowLatency) {
              pending_.GetLowLatency(task->task_node_.node_depth_).push(
                  PrivateTaskQueueEntry{task, &lane_info});
            } else {
              pending_.GetHighLatency(task->task_node_.node_depth_).push(
                  PrivateTaskQueueEntry{task, &lane_info});
            }
            ++pending;
          } else {
            EndTask(exec, task);
          }
        } else {
          break;
        }
      }
    }
    return pending;
  }

  /** Run an iteration over a particular queue */
  HSHM_ALWAYS_INLINE
  void PollPending(bool flushing) {
    // Poll low-latency tasks
    for (int depth = HSHM_WORKER_MAX_TASK_DEPTH - 1; depth >= 0; --depth) {
      PrivateTaskQueue &queue = pending_.GetLowLatency(depth);
      PollPrivateQueue(queue, flushing);
    }
    // Poll high-latency tasks
    for (int depth = HSHM_WORKER_MAX_TASK_DEPTH - 1; depth >= 0; --depth) {
      PrivateTaskQueue &queue = pending_.GetHighLatency(depth);
      PollPrivateQueue(queue, flushing);
    }
    // Poll long-running tasks
    PollPrivateQueue(pending_.GetLongRunning(), flushing);
  }

  /** Poll the set of tasks in the private queue */
  void PollPrivateQueue(PrivateTaskQueue &queue, bool flushing) {
    for (size_t i = queue.head_; i < queue.tail_; ++i) {
      PrivateTaskQueueEntry entry;
      queue.peek(i, entry);
      if (entry.task_.ptr_ != nullptr) {
        TaskState *exec = RunTask(*entry.lane_info_,
                                  entry.task_,
                                  entry.lane_info_->lane_id_,
                                  flushing);
        if (entry.task_->IsModuleComplete()) {
          EndTask(exec, entry.task_);
          queue.erase(i);
        }
      }
    }
  }

  /** Run a task */
  TaskState* RunTask(WorkEntry &lane_info,
                     LPointer<Task> task,
                     u32 lane_id,
                     bool flushing) {
    // Get the task state
    TaskState *exec = GetTaskState(task->task_state_);
    if (!exec) {
      HELOG(kWarning, "(node {}) Could not find the task state: {}",
            HRUN_CLIENT->node_id_, task->task_state_);
      return exec;
    }
    // Pack runtime context
    RunContext &rctx = task->ctx_;
    rctx.flush_ = &flush_;
    rctx.exec_ = exec;
    // Get task properties
    bitfield32_t props =
        GetTaskProperties(task.ptr_, exec, cur_time_,
                          lane_id, flushing);
    // Execute the task based on its properties
    ExecTask(lane_info, task.ptr_, rctx, exec, props);
    // Cleanup allocations
    if (task->IsModuleComplete()) {
      if (task->IsCoroutine() &&
          !props.Any(HSHM_WORKER_IS_REMOTE) &&
          !task->IsLaneAll()) {
        FreeStack(rctx.stack_ptr_);
      }
      RemoveTaskGroup(task.ptr_, exec,
                      lane_id,
                      props.Any(HSHM_WORKER_IS_REMOTE));
    }
    return exec;
  }

  /** Get the characteristics of a task */
  HSHM_ALWAYS_INLINE
  bitfield32_t GetTaskProperties(Task *&task,
                                 TaskState *&exec,
                                 hshm::Timepoint &cur_time,
                                 u32 lane_id,
                                 bool flushing) {
    bitfield32_t props;
    bool is_remote = task->domain_id_.IsRemote(
        HRUN_RPC->GetNumHosts(), HRUN_CLIENT->node_id_);
#ifdef HERMES_REMOTE_DEBUG
    if (task->task_state_ != HRUN_QM_CLIENT->admin_task_state_ &&
            !task->task_flags_.Any(TASK_REMOTE_DEBUG_MARK) &&
            task->method_ != TaskMethod::kConstruct &&
            HRUN_RUNTIME->remote_created_) {
          is_remote = true;
        }
#endif
    bool group_avail = CheckTaskGroup(task, exec,
                                      lane_id,
                                      task->task_node_,
                                      is_remote);
    bool should_run = task->ShouldRun(cur_time, flushing);
    if (is_remote) {
      props.SetBits(HSHM_WORKER_IS_REMOTE);
    }
    if (group_avail) {
      props.SetBits(HSHM_WORKER_GROUP_AVAIL);
    }
    if (should_run) {
      props.SetBits(HSHM_WORKER_SHOULD_RUN);
    }
    if (flushing) {
      props.SetBits(HSHM_WORKER_IS_FLUSHING);
    }
    if (task->IsLongRunning()) {
      props.SetBits(HSHM_WORKER_LONG_RUNNING);
    }
    return props;
  }
  
  /** Run an arbitrary task */
  HSHM_ALWAYS_INLINE
  bool ExecTask(WorkEntry &lane_info,
                Task *&task,
                RunContext &rctx,
                TaskState *&exec,
                bitfield32_t &props) {
    // Determine if a task should be executed
    if (task->IsRunDisabled() ||
        !props.All(HSHM_WORKER_GROUP_AVAIL | HSHM_WORKER_SHOULD_RUN)) {
      return false;
    }
    if (!task->IsLongRunning()) {
      work_ += 1;
    }
    // Flush tasks
    if (props.Any(HSHM_WORKER_IS_FLUSHING) && !task->IsFlush()) {
      if (task->IsLongRunning()) {
        exec->Monitor(MonitorMode::kFlushStat, task, rctx);
      } else {
        flush_.count_ += 1;
      }
    }
    // Monitoring callback
    if (!task->IsStarted()) {
      exec->Monitor(MonitorMode::kBeginTrainTime, task, rctx);
    }
    // Attempt to run the task if it's ready and runnable
    if (props.Any(HSHM_WORKER_IS_REMOTE)) {
      auto ids = HRUN_RUNTIME->ResolveDomainId(
          task->domain_id_);
      HRUN_REMOTE_QUEUE->Disperse(task, exec, ids);
      task->SetDisableRun();
    } else if (task->IsLaneAll()) {
      HRUN_REMOTE_QUEUE->DisperseLocal(task, exec,
                                       lane_info.queue_,
                                       lane_info.group_);
      task->SetDisableRun();
    } else if (task->IsCoroutine()) {
      ExecCoroutine(task, rctx);
    } else {
      exec->Run(task->method_, task, rctx);
      task->SetStarted();
    }
    // Monitoring callback
    if (task->IsModuleComplete()) {
      exec->Monitor(MonitorMode::kEndTrainTime, task, rctx);
    }
    task->DidRun(cur_time_);
    return !task->IsFlush();
  }

  /** Run a task */
  HSHM_ALWAYS_INLINE
  void ExecCoroutine(Task *&task, RunContext &rctx) {
    // If task isn't started, allocate stack pointer
    if (!task->IsStarted()) {
      rctx.stack_ptr_ = AllocateStack();
      if (rctx.stack_ptr_ == nullptr) {
        HELOG(kFatal, "The stack pointer of size {} is NULL",
              stack_size_, rctx.stack_ptr_);
      }
      rctx.jmp_.fctx = bctx::make_fcontext(
          (char *) rctx.stack_ptr_ + stack_size_,
          stack_size_, &Worker::CoroutineEntry);
      task->SetStarted();
    }
    // Jump to CoroutineEntry
    rctx.jmp_ = bctx::jump_fcontext(rctx.jmp_.fctx, task);
    // Rebuild stack pointer if coroutine is not done
    // NOTE(llogan): Not entirely sure if this is needed
//    if (!task->IsStarted()) {
//      rctx.jmp_.fctx = bctx::make_fcontext(
//          (char *) rctx.stack_ptr_ + stack_size_,
//          stack_size_, &Worker::CoroutineEntry);
//      task->SetStarted();
//    }
  }

  /** Run a coroutine */
  static void CoroutineEntry(bctx::transfer_t t) {
    Task *task = reinterpret_cast<Task*>(t.data);
    RunContext &rctx = task->ctx_;
    TaskState *&exec = rctx.exec_;
    rctx.jmp_ = t;
    exec->Run(task->method_, task, rctx);
    // NOTE(llogan): Not entirely sure if this is needed
    // task->UnsetStarted();
    task->Yield<TASK_YIELD_CO>();
  }

  /**===============================================================
   * Task Ordering and Completion
   * =============================================================== */

  /** Get task state */
  HSHM_ALWAYS_INLINE
  TaskState* GetTaskState(const TaskStateId &state_id) {
    auto it = state_map_.find(state_id);
    if (it == state_map_.end()) {
      TaskState *state = HRUN_TASK_REGISTRY->GetTaskState(state_id);
      if (state == nullptr) {
        return nullptr;
      }
      state_map_.emplace(state_id, state);
      return state_map_[state_id];
    }
    return it->second;
  }

  /** Check if two tasks can execute concurrently */
  // HSHM_ALWAYS_INLINE
  bool CheckTaskGroup(Task *task, TaskState *exec,
                      u32 lane_id,
                      TaskNode node, const bool &is_remote) {
    if (is_remote || task->IsStarted() || task->IsLaneAll()) {
      return true;
    }
    int ret = exec->GetGroup(task->method_, task, group_);
    if (ret == TASK_UNORDERED || task->IsUnordered()) {
//      HILOG(kDebug, "(node {}) Task {} is unordered, so count remains 0 worker={}",
//            HRUN_CLIENT->node_id_, task->task_node_, id_);
      return true;
    }

    // Ensure that concurrent requests are not serialized
    LocalSerialize srl(group_, false);
    srl << lane_id;

    auto it = group_map_.find(group_);
    if (it == group_map_.end()) {
      node.node_depth_ = 1;
      group_map_.emplace(group_, node);
//      HILOG(kDebug, "(node {}) Increasing depth of group {} to {} (worker={})",
//            HRUN_CLIENT->node_id_, std::hash<hshm::charbuf>{}(group_), node.node_depth_, id_);
      return true;
    }
    TaskNode &node_cmp = it->second;
    if (node_cmp.root_ == node.root_) {
      node_cmp.node_depth_ += 1;
//      HILOG(kDebug, "(node {}) Increasing depth of group {} to {} (worker={})",
//            HRUN_CLIENT->node_id_, std::hash<hshm::charbuf>{}(group_), node.node_depth_, id_);
      return true;
    }
    return false;
  }

  /** No longer serialize tasks of the same group */
  HSHM_ALWAYS_INLINE
  void RemoveTaskGroup(Task *task, TaskState *exec,
                       u32 lane_id, const bool &is_remote) {
    if (is_remote || task->IsLaneAll()) {
      return;
    }
    int ret = exec->GetGroup(task->method_, task, group_);
    if (ret == TASK_UNORDERED || task->IsUnordered()) {
//      HILOG(kDebug, "(node {}) Decreasing depth of group remains 0 (task_node={} worker={})",
//            HRUN_CLIENT->node_id_, task->task_node_, id_);
      return;
    }

    // Ensure that concurrent requests are not serialized
    LocalSerialize srl(group_, false);
    srl << lane_id;

    TaskNode &node_cmp = group_map_[group_];
    if (node_cmp.node_depth_ == 0) {
      HELOG(kFatal, "(node {}) Group {} depth is already 0 (task_node={} worker={})",
            HRUN_CLIENT->node_id_, std::hash<hshm::charbuf>{}(group_), task->task_node_, id_);
    }
    node_cmp.node_depth_ -= 1;
//    HILOG(kDebug, "(node {}) Decreasing depth of to {} (task_node={} worker={})",
//          HRUN_CLIENT->node_id_, node_cmp.node_depth_, task->task_node_, id_);
    if (node_cmp.node_depth_ == 0) {
      group_map_.erase(group_);
    }
  }

  /** Free a task when it is no longer needed */
  HSHM_ALWAYS_INLINE
  void EndTask(TaskState *exec, LPointer<Task> &task) {
    if (exec && task->IsFireAndForget()) {
      exec->Del(task->method_, task.ptr_);
    } else {
      task->SetComplete();
    }
  }
};

}  // namespace hrun

#endif  // HRUN_INCLUDE_HRUN_WORK_ORCHESTRATOR_WORKER_H_
