/* Author: Suki Sahota */
#include "config.h"
#include "globals.h"

#include "errno.h"

#include "util/init.h"
#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"

#include "proc/kthread.h"
#include "proc/proc.h"
#include "proc/sched.h"

#include "mm/slab.h"
#include "mm/page.h"

kthread_t *curthr; /* global */
static slab_allocator_t *kthread_allocator = NULL;

#ifdef __MTP__
/* Stuff for the reaper daemon, which cleans up dead detached threads */
static proc_t *reapd = NULL;
static kthread_t *reapd_thr = NULL;
static ktqueue_t reapd_waitq;
static list_t kthread_reapd_deadlist; /* Threads to be cleaned */

static void *kthread_reapd_run(int arg1, void *arg2);
#endif

void
kthread_init()
{
        kthread_allocator = slab_allocator_create("kthread", sizeof(kthread_t));
        KASSERT(NULL != kthread_allocator);
}

/**
 * Allocates a new kernel stack.
 *
 * @return a newly allocated stack, or NULL if there is not enough
 * memory available
 */
static char *
alloc_stack(void)
{
        /* extra page for "magic" data */
        char *kstack;
        int npages = 1 + (DEFAULT_STACK_SIZE >> PAGE_SHIFT);
        kstack = (char *)page_alloc_n(npages);

        return kstack;
}

/**
 * Frees a stack allocated with alloc_stack.
 *
 * @param stack the stack to free
 */
static void
free_stack(char *stack)
{
        page_free_n(stack, 1 + (DEFAULT_STACK_SIZE >> PAGE_SHIFT));
}

void
kthread_destroy(kthread_t *t)
{
        KASSERT(t && t->kt_kstack);
        free_stack(t->kt_kstack);
        if (list_link_is_linked(&t->kt_plink))
                list_remove(&t->kt_plink);

        slab_obj_free(kthread_allocator, t);
}

/*
 * Allocate a new stack with the alloc_stack function. The size of the
 * stack is DEFAULT_STACK_SIZE.
 *
 * Don't forget to initialize the thread context with the
 * context_setup function. The context should have the same pagetable
 * pointer as the process.
 */
kthread_t *
kthread_create(struct proc *p, kthread_func_t func, long arg1, void *arg2)
{
  /* Precondition KASSERT statements begin */
  KASSERT(NULL != p); /* the p argument of this function must be a valid process */
  /* Precondition KASSERT statements end */
  
  kthread_t *newthread = slab_obj_alloc(kthread_allocator);
  KASSERT(newthread != NULL && "Ran out of memory in kthread creation");

  void *ktstack = alloc_stack();
  
  KASSERT(NULL != ktstack && "Ran out of memory in thread context setup.");
  context_setup(&newthread->kt_ctx, func, (int) arg1, arg2, 
                ktstack, DEFAULT_STACK_SIZE, p->p_pagedir);
  
  newthread->kt_kstack = ktstack;
  KASSERT(newthread->kt_kstack != NULL);
  newthread->kt_retval = NULL; // No return value for this thread yet
  newthread->kt_errno = 0;
  newthread->kt_proc = p;
  newthread->kt_cancelled = 0;
  newthread->kt_wchan = NULL; // Thread is not blocked on any queue yet
  newthread->kt_state = KT_RUN;

  list_link_init(&newthread->kt_qlink);
  list_insert_tail(&p->p_threads, &(newthread->kt_plink));

  return newthread;
}

/*
 * If the thread to be cancelled is the current thread, this is
 * equivalent to calling kthread_exit. Otherwise, the thread is
 * sleeping (either on a waitqueue or a runqueue) 
 * and we need to set the cancelled and retval fields of the
 * thread. On wakeup, threads should check their cancelled fields and
 * act accordingly. 
 *
 * If the thread's sleep is cancellable, cancelling the thread should
 * wake it up from sleep.
 *
 * If the thread's sleep is not cancellable, we do nothing else here.
 *
 */
void
kthread_cancel(kthread_t *kthr, void *retval)
{
  /* Precondition KASSERT statements begin */
  KASSERT(NULL != kthr); /* the kthr argument of this function must be a valid thread */
  /* Precondition KASSERT statements end */

  // if we are self-terminating curthr
  if (curthr == kthr) {
    kthread_exit(retval);
    panic("Should never get here! kthread_exit() will kill thread.");
  } 

  kthr->kt_retval = retval;
  
  // sched_cancel sets cancel flag and checks if in cancellable sleep
  sched_cancel(kthr);
}

/*
 * You need to set the thread's retval field and alert the current
 * process that a thread is exiting via proc_thread_exited. You should
 * refrain from setting the thread's state to KT_EXITED until you are
 * sure you won't make any more blocking calls before you invoke the
 * scheduler again.
 *
 * It may seem unneccessary to push the work of cleaning up the thread
 * over to the process. However, if you implement MTP, a thread
 * exiting does not necessarily mean that the process needs to be
 * cleaned up.
 *
 * The void * type of retval is simply convention and does not necessarily
 * indicate that retval is a pointer
 */
void
kthread_exit(void *retval)
{
  curthr->kt_retval = retval;
  /* Middle KASSERT statements begin */
  KASSERT(!curthr->kt_wchan); /* curthr should not be (sleeping) in any queue */
  /* this thread must not be part of any list */  
  KASSERT(!curthr->kt_qlink.l_next && !curthr->kt_qlink.l_prev); 
  KASSERT(curthr->kt_proc == curproc); /* this thread belongs to curproc */
  /* Middle KASSERT statements end */

  curthr->kt_state = KT_EXITED; // set zombie flag before making zombie
  proc_thread_exited(retval); // make zombie here
  panic("Should never get here! proc_thread_exited() will kill thread.");
}

/*
 * The new thread will need its own context and stack. Think carefully
 * about which fields should be copied and which fields should be
 * freshly initialized.
 *
 * You do not need to worry about this until VM.
 */
kthread_t *
kthread_clone(kthread_t *thr)
{
  /* Begin precondition */
  KASSERT(KT_RUN == thr->kt_state); /* the thread you are cloning must be in the running or runnable state */
  /* End precondition */

  kthread_t *newthr = slab_obj_alloc(kthread_allocator);
  KASSERT(newthr != NULL && "Ran out of memory in kthread clone");

  void *contextstack = alloc_stack();
  KASSERT(NULL != contextstack &&
          "Ran out of memory in kthread clone context setup.");

  // Cloned thread needs its own context
  // Set rest of context_t in fork
  newthr->kt_ctx.c_kstack = (uint32_t) contextstack; // assign new stack
  newthr->kt_ctx.c_kstacksz = DEFAULT_STACK_SIZE;

  // Cloned thread needs its own kernel stack
  void *kernelstack = alloc_stack(); 
  KASSERT(NULL != kernelstack &&
          "Ran out of memory in kthread clone stack setup.");

  newthr->kt_kstack = kernelstack; // Set up in fork 

  newthr->kt_retval = thr->kt_retval;
  newthr->kt_errno = thr->kt_errno;
  newthr->kt_proc = NULL; // will be set in fork
  newthr->kt_cancelled = thr->kt_cancelled;
  newthr->kt_wchan = thr->kt_wchan; // Queue that thr is blocked on
  newthr->kt_state = KT_RUN;

  // Initialize list links
  list_link_init(&newthr->kt_qlink);
  list_link_init(&newthr->kt_plink);

  /* Begin postcondition: let newthr be the new thread */
  KASSERT(KT_RUN == newthr->kt_state); /* new thread starts in the runnable state */
  /* End postcondition */

  return newthr;
}

/*
 * The following functions will be useful if you choose to implement
 * multiple kernel threads per process. This is strongly discouraged
 * unless your weenix is perfect.
 */
#ifdef __MTP__
int
kthread_detach(kthread_t *kthr)
{
        NOT_YET_IMPLEMENTED("MTP: kthread_detach");
        return 0;
}

int
kthread_join(kthread_t *kthr, void **retval)
{
        NOT_YET_IMPLEMENTED("MTP: kthread_join");
        return 0;
}

/* ------------------------------------------------------------------ */
/* -------------------------- REAPER DAEMON ------------------------- */
/* ------------------------------------------------------------------ */
static __attribute__((unused)) void
kthread_reapd_init()
{
        NOT_YET_IMPLEMENTED("MTP: kthread_reapd_init");
}
init_func(kthread_reapd_init);
init_depends(sched_init);

void
kthread_reapd_shutdown()
{
        NOT_YET_IMPLEMENTED("MTP: kthread_reapd_shutdown");
}

static void *
kthread_reapd_run(int arg1, void *arg2)
{
        NOT_YET_IMPLEMENTED("MTP: kthread_reapd_run");
        return (void *) 0;
}
#endif
