#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/float.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b
/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
static struct list sleep_list;//add sleep_list
static int next_tick_to_awake = 0;//save next time at wake up
static int load_avg;
static struct thread *idle_thread;

/* list of all processes.  processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* lock used by allocate_tid(). */
static struct lock tid_lock;

/* stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* return address. */
    thread_func *function;      /* function to call. */
    void *aux;                  /* auxiliary data for function. */
  };

/* statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* scheduling. */
#define time_slice 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* if false (default), use round-robin scheduler.
   if true, use multi-level feedback queue scheduler.
   controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *);
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* initializes the threading system by transforming the code
   that's currently running into a thread.  this can't work in
   general and it is possible in this case only because loader.s
   was careful to put the bottom of the stack at a page boundary.

   also initializes the run queue and the tid lock.

   after calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   it is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT(intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&sleep_list);//initialize list
  list_init (&all_list);

  /* set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

}

/* starts preemptive thread scheduling by enabling interrupts.
   also creates the idle thread. */
void
thread_start (void) 
{
  /* create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  load_avg = 0;

  /* start preemptive thread scheduling. */
  intr_enable ();

  /* wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* called by the timer interrupt handler at each timer tick.
   thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef userprog
  else if (t->pagedir != null)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* enforce preemption. */
  if (++thread_ticks >= time_slice)
    intr_yield_on_return ();
}

/* prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* creates a new kernel thread named name with the given initial
   priority, which executes function passing aux as the argument,
   and adds it to the ready queue.  returns the thread identifier
   for the new thread, or tid_error if creation fails.

   if thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  it could even exit
   before thread_create() returns.  contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  use a semaphore or some other form of
   synchronization if you need to ensure ordering.


   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */




tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT(function != NULL);

  /* allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* prepare thread for first run by initializing its stack.
     do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  t->parent = thread_current();
  t->load_success = false;

  /* add to run queue. */
  thread_unblock (t);
  priority_yield ();
  return tid;
}

/* puts the current thread to sleep.  it will not be scheduled
   again until awoken by thread_unblock().

   this function must be called with interrupts turned off.  it
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT(!intr_context ());
  ASSERT(intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* transitions a blocked thread t to the ready-to-run state.
   this is an error if t is not blocked.  (use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT(is_thread (t));

  old_level = intr_disable ();

  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, compare_thread_priority, 0);
//  list_push_back (&ready_list, &t->elem);
//  this is original code

  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    list_insert_ordered (&ready_list, &cur->elem, compare_thread_priority, 0);
    // original code
    // list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}


/* function for solving project 1 */

void next_wakeup_compare(int ticks)
{
    if(next_tick_to_awake > ticks)
        next_tick_to_awake = ticks;
}//compare with ticks and change it

int get_next_tick()
{
    return next_tick_to_awake;
}

void thread_goto_sleep(int ticks)
{
    struct thread *gotosleep;
    enum intr_level old_level;
    old_level = intr_disable();//don't interrupt

    gotosleep = thread_current();
    if(gotosleep == idle_thread)
        return;

    gotosleep->waketime = ticks;
    list_push_back(&sleep_list, &gotosleep->elem);

    thread_block();
    intr_set_level(old_level);

    next_wakeup_compare(ticks);
}

void thread_goto_ready(int ticks)
{
    struct list_elem *e = list_begin(&sleep_list);

    while(e != list_end(&sleep_list))
    {
        struct thread *t = list_entry(e, struct thread, elem);//point to e thread

        if(ticks >= t->waketime)
        {
            e = list_remove(&t->elem);
            thread_unblock(t);// state of t is ready!
        }
        else
        {
            e = list_next(e);
            next_wakeup_compare(t->waketime);
        }
    }

}


bool compare_thread_priority(struct list_elem *e1, struct list_elem *e2, void *aux UNUSED){
    //
    struct thread *t1 = list_entry(e1, struct thread, elem);
    struct thread *t2 = list_entry(e2, struct thread, elem);
    return (t1->priority > t2->priority);
}


void priority_yield(void){
    struct thread *ct = thread_current();

    if(list_empty (&ready_list)){
        return;
    }

    if(ct->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority){
        thread_yield ();
    }

};

struct thread *insert_donators(struct thread *t){
    list_insert_ordered(&thread_current ()->donators, &t->elem, compare_thread_priority, 0);
}

struct thread *pop_donators() {
    return list_pop_front (&thread_current()->donators);
}

void ready_list_sort(void) {
    list_sort(&ready_list, compare_thread_priority, 0);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{

        struct thread *ct = thread_current ();
        if (ct->donated_level || ct->is_donating){
            ct->priority_after = new_priority;
            return;
        } else {
            thread_current ()->priority = new_priority;
            if(list_empty (&ready_list))
                return;

            priority_yield ();
        } 
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{

    if(thread_mlfqs)
        return;

  struct thread *cur = thread_current();
  cur -> nice = nice;
  mlfqs_priority_change(cur);
  //maxpriority_check();
}
void mlfqs_priority_change(struct thread *t)
{
    if(t == idle_thread)
        return;

    
    int term1 = DIV_INT( t->recent_cpu, 4);
    int term2 = 2*t->nice;
    t->priority = PRI_MAX - FP_TO_INT_ROUND(term1) - term2;
    if (t->priority <= PRI_MIN)
    {
        t->priority = PRI_MIN;
    }
    if (t->priority >= PRI_MAX)
    {
        t->priority = PRI_MAX;
    }
}
void mlfqs_recent_cpu_change(struct thread *t)
{
    int term1, term2, term3; 
    if(t == idle_thread)
        return;
    term1 = MULT_INT(load_avg, 2);
    term2 = DIV_FP(term1, ADD_INT(term1, 1) );
    term3 = MULT_FP(term2, t->recent_cpu);
    t->recent_cpu = ADD_INT (term3, t->nice);
}
void mlfqs_load_avg_change()
{
    int term2 = list_size(&ready_list);
    if (thread_current() != idle_thread)
    {
          term2++;
    }
    
  
    load_avg = MULT_FP(DIV_INT(INT_TO_FP(59), 60), load_avg) + MULT_INT(DIV_INT(INT_TO_FP(1), 60), term2);
    ASSERT (load_avg >= 0)
}
void mlfqs_inc(struct thread *t)
{
    if(t == idle_thread)
        return;
    
    t->recent_cpu = t->recent_cpu + FRACTION;
}   
void mlfqs_all_recent_cpu_change()
{
    struct thread *t;
    struct list_elem *elem;
    for(elem = list_begin(&all_list); elem != list_end(&all_list); elem = list_next(elem))
    {
        t= list_entry(elem, struct thread, allelem);
        mlfqs_recent_cpu_change(t);
    }

}

void mlfqs_all_priority_change(void){
    struct thread *t;
    struct list_elem *elem;
    for(elem = list_begin(&all_list); elem != list_end(&all_list); elem = list_next(elem))
    {
        t= list_entry(elem, struct thread, allelem);
        mlfqs_priority_change(t);
    }
    
    if(list_empty(&ready_list))
        return;

    list_sort (&ready_list, compare_thread_priority, 0);
    //maxpriority_check();
};

void test_max_priority(void)
{
    if(!list_empty(&ready_list))
    {
        if(thread_get_priority() < list_entry(ready_list.head.next, struct thread, elem)->priority)
        {
                thread_yield();
        }
    }
}
void maxpriority_check()
{
    int temp_pri =thread_current()->priority;
    struct thread *t = NULL;
    int ready_pri = 0;
              
    if ( list_empty(&ready_list) ){ 
        return;
    }//if empty didn't anything
                    

    t = list_entry(list_front(&ready_list),struct thread,elem);
    if(t!=NULL){
        ready_pri = t->priority; 
    }
    if (intr_context())
      {
      thread_ticks++;
    if ( temp_pri < ready_pri || (thread_ticks >= 4 && temp_pri == ready_pri) )
      {
      intr_yield_on_return();
      }
      return;
      }
                        
     if (temp_pri < ready_pri)
        thread_yield();
}


/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current() -> nice; 
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
    enum intr_level old_level = intr_disable();
    int i = FP_TO_INT_ROUND (MULT_INT (load_avg, 100));
    intr_set_level(old_level);
    return i;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return FP_TO_INT_ROUND( MULT_INT(thread_current()->recent_cpu, 100) );
}
/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}
/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->origin_priority = priority;
  t->donated_level = 0;
  t->is_donating = false;
  t->magic = THREAD_MAGIC;
  t->receiver = NULL;
  t->wait_lock = NULL;
  t->priority_after = -1;
  list_init (&t->donators);
  list_push_back (&all_list, &t->allelem);

  if(thread_mlfqs){

      list_init(&t->child_list);
      if (t == initial_thread)
      {
          t->nice = 0;
          t->recent_cpu = 0;
      }
      else
      {
          t->nice = thread_get_nice ();
          t->recent_cpu = thread_get_recent_cpu ();
      }
      mlfqs_priority_change(t);
  }
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
