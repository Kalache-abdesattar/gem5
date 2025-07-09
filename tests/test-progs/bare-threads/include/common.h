struct __pthread_internal_list
{
    struct __pthread_internal_list *__prev;
    struct __pthread_internal_list *__next;
};


typedef union
{
  struct __pthread_mutex_s
  {
    int __lock;
    unsigned int __count;
    int __owner;
    unsigned int __nusers;
    int __kind;
    short __spins;
    short __elision;
    struct __pthread_internal_list __list;
  } __data;
  char __size[40];
  long int __align;
} pthread_mutex_t;



class mutex {
    pthread_mutex_t _M_mutex;

public:
    mutex() { pthread_mutex_init(&_M_mutex, nullptr); }
    ~mutex() { pthread_mutex_destroy(&_M_mutex); }
    void lock() { pthread_mutex_lock(&_M_mutex); }
    void unlock() { pthread_mutex_unlock(&_M_mutex); }
};

