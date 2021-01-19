struct platform_work_queue_entry {
  platform_work_queue_callback *callback;
  void *data;
};

struct platform_work_queue {
  u32 volatile completion_goal;
  u32 volatile completion_count;

  u32 volatile next_entry_to_read;
  u32 volatile next_entry_to_write;

  HANDLE semaphore_handle;

  platform_work_queue_entry entries[256];
};

struct win32_thread_startup{
  platform_work_queue *queue;
};
