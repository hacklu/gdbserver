#include "server.h"
#include "target.h"

#include "gnu-low.h"

#include <limits.h>
#include <sys/ptrace.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include "gdb_wait.h"
#include <signal.h>

#include "msg_reply_S.h"
#include "exc_request_S.h"
#include "process_reply_S.h"
#include "notify_S.h"

/* hacklu: this should move into gnu-i386-low.c etc. */
/* Defined in auto-generated file i386.c.  */
extern void init_registers_i386 (void);
extern const struct target_desc *tdesc_i386;

const struct target_desc *gnu_tdesc;
/* If we've sent a proc_wait_request to the proc server, the pid of the
   process we asked about.  We can only ever have one outstanding.  */
int proc_wait_pid = 0;

/* The number of wait requests we've sent, and expect replies from.  */
int proc_waits_pending = 0;

int using_threads=1; 

struct inf* gnu_current_inf=NULL;
struct inf* waiting_inf=NULL;
static process_t proc_server = MACH_PORT_NULL;
static int next_thread_id = 1;
ptid_t inferior_ptid;

/* Evaluate RPC_EXPR in a scope with the variables MSGPORT and REFPORT bound
   to INF's msg port and task port respectively.  If it has no msg port,
   EIEIO is returned.  INF must refer to a running process!  */
#define INF_MSGPORT_RPC(inf, rpc_expr) \
  HURD_MSGPORT_RPC (proc_getmsgport (proc_server, inf->pid, &msgport), \
		    (refport = inf->task->port, 0), 0, \
		    msgport ? (rpc_expr) : EIEIO)

/* Like INF_MSGPORT_RPC, but will also resume the signal thread to ensure
   there's someone around to deal with the RPC (and resuspend things
   afterwards).  This effects INF's threads' resume_sc count.  */
#define INF_RESUME_MSGPORT_RPC(inf, rpc_expr) \
  (inf_set_threads_resume_sc_for_signal_thread (inf) \
   ? ({ error_t __e; \
	inf_resume (inf); \
	__e = INF_MSGPORT_RPC (inf, rpc_expr); \
	inf_suspend (inf); \
	__e; }) \
   : EIEIO)
static struct target_ops gnu_target_ops;

struct process_info_private
{
	struct inf * inf;
};

#ifndef PIDGET
#define PIDGET(PTID) (ptid_get_pid (PTID))
#define TIDGET(PTID) (ptid_get_lwp (PTID))
#define MERGEPID(PID, TID) ptid_build (PID, TID, 0)
#endif

static int debug_flags=0;

#define inf_debug(_inf, msg, args...) \
  do { struct inf *__inf = (_inf); \
       debug ("{inf %d %s}: " msg, __inf->pid, \
       host_address_to_string (__inf) , ##args); } while (0)

#define proc_debug(_proc, msg, args...) \
  do { struct proc *__proc = (_proc); \
       debug ("{proc %d/%d %s}: " msg, \
	      __proc_pid (__proc), __proc->tid, \
	      host_address_to_string (__proc) , ##args); } while (0)

#define debug(msg, args...) \
 do { if (debug_flags) \
        printf ("%s:%d: " msg "\r\n", \
			    __FILE__ , __LINE__ , ##args); } while (0)
#ifndef safe_strerror 
#define safe_strerror(err) \
	"XXXX"
#endif

static void
gnu_debug (char *string, ...)
{
  va_list args;

  if (!debug_flags)
    return;
  va_start (args, string);
  fprintf (stderr, "DEBUG(gnu): ");
  vfprintf (stderr, string, args);
  fprintf (stderr, "\n");
  va_end (args);
}

/* Set up the thread resume_sc's so that only the signal thread is running
   (plus whatever other thread are set to always run).  Returns true if we
   did so, or false if we can't find a signal thread.  */
int inf_set_threads_resume_sc_for_signal_thread (struct inf *inf)
{
  if (inf->signal_thread)
    {
      inf_set_threads_resume_sc (inf, inf->signal_thread, 0);
      return 1;
    }
  else
    return 0;
}

/* Sets the resume_sc of each thread in inf.  That of RUN_THREAD is set to 0,
   and others are set to their run_sc if RUN_OTHERS is true, and otherwise
   their pause_sc.  */
void
inf_set_threads_resume_sc (struct inf *inf,
			   struct proc *run_thread, int run_others)
{
  struct proc *thread;

  inf_update_procs (inf);
  for (thread = inf->threads; thread; thread = thread->next)
    if (thread == run_thread)
      thread->resume_sc = 0;
    else if (run_others)
      thread->resume_sc = thread->run_sc;
    else
      thread->resume_sc = thread->pause_sc;
}
void inf_clear_wait (struct inf *inf)
{
  inf_debug (inf, "clearing wait");
  inf->wait.status.kind = TARGET_WAITKIND_SPURIOUS;
  inf->wait.thread = 0;
  inf->wait.suppress = 0;
  if (inf->wait.exc.handler != MACH_PORT_NULL)
    {
      mach_port_deallocate (mach_task_self (), inf->wait.exc.handler);
      inf->wait.exc.handler = MACH_PORT_NULL;
    }
  if (inf->wait.exc.reply != MACH_PORT_NULL)
    {
      mach_port_deallocate (mach_task_self (), inf->wait.exc.reply);
      inf->wait.exc.reply = MACH_PORT_NULL;
    }
}

int __proc_pid (struct proc *proc)
{
  return proc->inf->pid;
}

static ptid_t gnu_ptid_build(int pid,long lwp,long tid)
{
	return ptid_build(pid,tid,0);
}
static long gnu_get_tid(ptid_t ptid)
{
	return ptid_get_lwp(ptid);
}

int proc_update_sc (struct proc *proc)
{
  int running;
  int err = 0;
  int delta = proc->sc - proc->cur_sc;

  if (delta)
    gnu_debug ("sc: %d --> %d", proc->cur_sc, proc->sc);

  if (proc->sc == 0 && proc->state_changed)
    /* Since PROC may start running, we must write back any state changes.  */
    {
      gdb_assert (proc_is_thread (proc));
      err = thread_set_state (proc->port, THREAD_STATE_FLAVOR,
			 (thread_state_t) &proc->state, THREAD_STATE_SIZE);
      if (!err)
	proc->state_changed = 0;
    }

  if (delta > 0)
    {
      while (delta-- > 0 && !err)
	{
	  if (proc_is_task (proc))
	    err = task_suspend (proc->port);
	  else
	    err = thread_suspend (proc->port);
	}
    }
  else
    {
      while (delta++ < 0 && !err)
	{
	  if (proc_is_task (proc))
	    err = task_resume (proc->port);
	  else
	    err = thread_resume (proc->port);
	}
    }
  if (!err)
    proc->cur_sc = proc->sc;

  /* If we got an error, then the task/thread has disappeared.  */
  running = !err && proc->sc == 0;

  proc_debug (proc, "is %s", err ? "dead" : running ? "running" : "suspended");
  if (err)
    proc_debug (proc, "err = %s", safe_strerror (err));

  if (running)
    {
      proc->aborted = 0;
      proc->state_valid = proc->state_changed = 0;
      proc->fetched_regs = 0;
    }

  return running;
}

error_t proc_get_exception_port (struct proc * proc, mach_port_t * port)
{
  if (proc_is_task (proc))
    return task_get_exception_port (proc->port, port);
  else
    return thread_get_exception_port (proc->port, port);
}
static mach_port_t _proc_get_exc_port (struct proc *proc)
{
  mach_port_t exc_port;
  error_t err = proc_get_exception_port (proc, &exc_port);

  if (err)
    /* PROC must be dead.  */
    {
      if (proc->exc_port)
	mach_port_deallocate (mach_task_self (), proc->exc_port);
      proc->exc_port = MACH_PORT_NULL;
      if (proc->saved_exc_port)
	mach_port_deallocate (mach_task_self (), proc->saved_exc_port);
      proc->saved_exc_port = MACH_PORT_NULL;
    }

  return exc_port;
}
#if 1
void inf_set_traced (struct inf *inf, int on)
{
  if (on == inf->traced)
    return;
  
  if (inf->task && !inf->task->dead)
    /* Make it take effect immediately.  */
    {
      sigset_t mask = on ? ~(sigset_t) 0 : 0;
      error_t err =
	INF_RESUME_MSGPORT_RPC (inf, msg_set_init_int (msgport, refport,
						       INIT_TRACEMASK, mask));

      if (err == EIEIO)
	{
	  /*if (on)*/
	    /*warning (_("Can't modify tracing state for pid %d: %s"),*/
		     /*inf->pid, "No signal thread");*/
	  inf->traced = on;
	}
      else if (err)
	      ;
	/*warning (_("Can't modify tracing state for pid %d: %s"),*/
		 /*inf->pid, safe_strerror (err));*/
      else
	inf->traced = on;
    }
  else
    inf->traced = on;
}
#endif

/* Makes all the real suspend count deltas of all the procs in INF
   match the desired values.  Careful to always do thread/task suspend
   counts in the safe order.  Returns true if at least one thread is
   thought to be running.  */
int
inf_update_suspends (struct inf *inf)
{
  struct proc *task = inf->task;

  /* We don't have to update INF->threads even though we're iterating over it
     because we'll change a thread only if it already has an existing proc
     entry.  */
  inf_debug (inf, "updating suspend counts");

  if (task)
    {
      struct proc *thread;
      int task_running = (task->sc == 0), thread_running = 0;

      if (task->sc > task->cur_sc)
	/* The task is becoming _more_ suspended; do before any threads.  */
	task_running = proc_update_sc (task);

      if (inf->pending_execs)
	/* When we're waiting for an exec, things may be happening behind our
	   back, so be conservative.  */
	thread_running = 1;

      /* Do all the thread suspend counts.  */
      for (thread = inf->threads; thread; thread = thread->next)
	thread_running |= proc_update_sc (thread);

      if (task->sc != task->cur_sc)
	/* We didn't do the task first, because we wanted to wait for the
	   threads; do it now.  */
	task_running = proc_update_sc (task);

      inf_debug (inf, "%srunning...",
		 (thread_running && task_running) ? "" : "not ");

      inf->running = thread_running && task_running;

      /* Once any thread has executed some code, we can't depend on the
         threads list any more.  */
      if (inf->running)
	inf->threads_up_to_date = 0;

      return inf->running;
    }

  return 0;
}
void proc_abort (struct proc *proc, int force)
{
  gdb_assert (proc_is_thread (proc));

  if (!proc->aborted)
    {
      struct inf *inf = proc->inf;
      int running = (proc->cur_sc == 0 && inf->task->cur_sc == 0);

      if (running && force)
	{
	  proc->sc = 1;
	  inf_update_suspends (proc->inf);
	  running = 0;
	  /*warning (_("Stopped %s."), proc_string (proc));*/
	}
      else if (proc == inf->wait.thread && inf->wait.exc.reply && !force)
	/* An exception is pending on PROC, which don't mess with.  */
	running = 1;

      if (!running)
	/* We only abort the thread if it's not actually running.  */
	{
	  thread_abort (proc->port);
	  proc_debug (proc, "aborted");
	  proc->aborted = 1;
	}
      else
	proc_debug (proc, "not aborting");
    }
}

thread_state_t proc_get_state (struct proc *proc, int will_modify)
{
  int was_aborted = proc->aborted;

  proc_debug (proc, "updating state info%s",
	      will_modify ? " (with intention to modify)" : "");

  proc_abort (proc, will_modify);

  if (!was_aborted && proc->aborted)
    /* PROC's state may have changed since we last fetched it.  */
    proc->state_valid = 0;

  if (!proc->state_valid)
    {
      mach_msg_type_number_t state_size = THREAD_STATE_SIZE;
      error_t err =
	thread_get_state (proc->port, THREAD_STATE_FLAVOR,
			  (thread_state_t) &proc->state, &state_size);

      proc_debug (proc, "getting thread state");
      proc->state_valid = !err;
    }

  if (proc->state_valid)
    {
      if (will_modify)
	proc->state_changed = 1;
      return (thread_state_t) &proc->state;
    }
  else
    return 0;
}

void proc_steal_exc_port (struct proc *proc, mach_port_t exc_port)
{
  mach_port_t cur_exc_port = _proc_get_exc_port (proc);

  if (cur_exc_port)
    {
      error_t err = 0;

      proc_debug (proc, "inserting exception port: %d", exc_port);

      if (cur_exc_port != exc_port)
	/* Put in our exception port.  */
	err = proc_set_exception_port (proc, exc_port);

      if (err || cur_exc_port == proc->exc_port)
	/* We previously set the exception port, and it's still set.  So we
	   just keep the old saved port which is what the proc set.  */
	{
	  if (cur_exc_port)
	    mach_port_deallocate (mach_task_self (), cur_exc_port);
	}
      else
	/* Keep a copy of PROC's old exception port so it can be restored.  */
	{
	  if (proc->saved_exc_port)
	    mach_port_deallocate (mach_task_self (), proc->saved_exc_port);
	  proc->saved_exc_port = cur_exc_port;
	}

      proc_debug (proc, "saved exception port: %d", proc->saved_exc_port);

      if (!err)
	proc->exc_port = exc_port;
      /*else*/
	/*warning (_("Error setting exception port for %s: %s"),*/
		 /*proc_string (proc), safe_strerror (err));*/
    }
}
int proc_trace (struct proc *proc, int set)
{
  thread_state_t state = proc_get_state (proc, 1);

  if (!state)
    return 0;			/* The thread must be dead.  */

  proc_debug (proc, "tracing %s", set ? "on" : "off");

  if (set)
    {
      /* XXX We don't get the exception unless the thread has its own
         exception port????  */
      if (proc->exc_port == MACH_PORT_NULL)
	proc_steal_exc_port (proc, proc->inf->event_port);
      THREAD_STATE_SET_TRACED (state);
    }
  else
    THREAD_STATE_CLEAR_TRACED (state);

  return 1;
}
error_t proc_set_exception_port (struct proc * proc, mach_port_t port)
{
  proc_debug (proc, "setting exception port: %d", port);
  if (proc_is_task (proc))
    return task_set_exception_port (proc->port, port);
  else
    return thread_set_exception_port (proc->port, port);
}
void proc_restore_exc_port (struct proc *proc)
{
  mach_port_t cur_exc_port = _proc_get_exc_port (proc);

  if (cur_exc_port)
    {
      error_t err = 0;

      proc_debug (proc, "restoring real exception port");

      if (proc->exc_port == cur_exc_port)
	/* Our's is still there.  */
	err = proc_set_exception_port (proc, proc->saved_exc_port);

      if (proc->saved_exc_port)
	mach_port_deallocate (mach_task_self (), proc->saved_exc_port);
      proc->saved_exc_port = MACH_PORT_NULL;

      if (!err)
	proc->exc_port = MACH_PORT_NULL;
      else
	gnu_debug("Error setting exception port\n");
    }
}
void inf_set_step_thread (struct inf *inf, struct proc *thread)
{
  gdb_assert (!thread || proc_is_thread (thread));

  /*if (thread)*/
    /*inf_debug (inf, "setting step thread: %d/%d", inf->pid, thread->tid);*/
  /*else*/
    /*inf_debug (inf, "clearing step thread");*/

  if (inf->step_thread != thread)
    {
      if (inf->step_thread && inf->step_thread->port != MACH_PORT_NULL)
	if (!proc_trace (inf->step_thread, 0))
	  return;
      if (thread && proc_trace (thread, 1))
	inf->step_thread = thread;
      else
	inf->step_thread = 0;
    }
}
struct proc * _proc_free (struct proc *proc)
{
  struct inf *inf = proc->inf;
  struct proc *next = proc->next;

  if (proc == inf->step_thread)
    /* Turn off single stepping.  */
    inf_set_step_thread (inf, 0);
  if (proc == inf->wait.thread)
    inf_clear_wait (inf);
  if (proc == inf->signal_thread)
    inf->signal_thread = 0;

  if (proc->port != MACH_PORT_NULL)
    {
      if (proc->exc_port != MACH_PORT_NULL)
	/* Restore the original exception port.  */
	proc_restore_exc_port (proc);
      if (proc->cur_sc != 0)
	/* Resume the thread/task.  */
	{
	  proc->sc = 0;
	  proc_update_sc (proc);
	}
      mach_port_deallocate (mach_task_self (), proc->port);
    }

  xfree (proc);
  return next;
}

struct proc * make_proc (struct inf *inf, mach_port_t port, int tid)
{
  error_t err;
  mach_port_t prev_port = MACH_PORT_NULL;
  struct proc *proc = xmalloc (sizeof (struct proc));

  proc->port = port;
  proc->tid = tid;
  proc->inf = inf;
  proc->next = 0;
  proc->saved_exc_port = MACH_PORT_NULL;
  proc->exc_port = MACH_PORT_NULL;

  proc->sc = 0;
  proc->cur_sc = 0;

  /* Note that these are all the values for threads; the task simply uses the
     corresponding field in INF directly.  */
  proc->run_sc = inf->default_thread_run_sc;
  proc->pause_sc = inf->default_thread_pause_sc;
  proc->detach_sc = inf->default_thread_detach_sc;
  proc->resume_sc = proc->run_sc;

  proc->aborted = 0;
  proc->dead = 0;
  proc->state_valid = 0;
  proc->state_changed = 0;

  proc_debug (proc, "is new");

  /* Get notified when things die.  */
  err =
    mach_port_request_notification (mach_task_self (), port,
				    MACH_NOTIFY_DEAD_NAME, 1,
				    inf->event_port,
				    MACH_MSG_TYPE_MAKE_SEND_ONCE,
				    &prev_port);
  if (err)
    warning (_("Couldn't request notification for port %d: %s"),
	     port, safe_strerror (err));
  else
    {
      proc_debug (proc, "notifications to: %d", inf->event_port);
      if (prev_port != MACH_PORT_NULL)
	mach_port_deallocate (mach_task_self (), prev_port);
    }

  if (inf->want_exceptions)
    {
      if (proc_is_task (proc))
	/* Make the task exception port point to us.  */
	proc_steal_exc_port (proc, inf->event_port);
      else
	/* Just clear thread exception ports -- they default to the
           task one.  */
	proc_steal_exc_port (proc, MACH_PORT_NULL);
    }

  return proc;
}

void inf_validate_procs (struct inf *inf)
{
  thread_array_t threads;
  mach_msg_type_number_t num_threads, i;
  struct proc *task = inf->task;

  /* If no threads are currently running, this function will guarantee that
     things are up to date.  The exception is if there are zero threads --
     then it is almost certainly in an odd state, and probably some outside
     agent will create threads.  */
  inf->threads_up_to_date = inf->threads ? !inf->running : 0;

  if (task)
    {
      error_t err = task_threads (task->port, &threads, &num_threads);

      inf_debug (inf, "fetching threads");
      if (err)
	/* TASK must be dead.  */
	{
	  task->dead = 1;
	  task = 0;
	}
    }

  if (!task)
    {
      num_threads = 0;
      inf_debug (inf, "no task");
    }

  {
    /* Make things normally linear.  */
    mach_msg_type_number_t search_start = 0;
    /* Which thread in PROCS corresponds to each task thread, & the task.  */
    struct proc *matched[num_threads + 1];
    /* The last thread in INF->threads, so we can add to the end.  */
    struct proc *last = 0;
    /* The current thread we're considering.  */
    struct proc *thread = inf->threads;

    memset (matched, 0, sizeof (matched));

    while (thread)
      {
	mach_msg_type_number_t left;

	for (i = search_start, left = num_threads; left; i++, left--)
	  {
	    if (i >= num_threads)
	      i -= num_threads;	/* I wrapped around.  */
	    if (thread->port == threads[i])
	      /* We already know about this thread.  */
	      {
		matched[i] = thread;
		last = thread;
		thread = thread->next;
		search_start++;
		break;
	      }
	  }

	if (!left)
	  {
	    proc_debug (thread, "died!");

	    ptid_t ptid;
	    ptid = gnu_ptid_build (inf->pid, 0, thread->tid);
	    if(find_thread_ptid(ptid))
		    remove_thread(find_thread_ptid(ptid));

	    thread->port = MACH_PORT_NULL;
	    thread = _proc_free (thread);	/* THREAD is dead.  */
	    if (last)
	      last->next = thread;
	    else
	      inf->threads = thread;
	  }
      }

    for (i = 0; i < num_threads; i++)
      {
	if (matched[i])
	  /* Throw away the duplicate send right.  */
	  mach_port_deallocate (mach_task_self (), threads[i]);
	else
	  /* THREADS[I] is a thread we don't know about yet!  */
	  {
	    ptid_t ptid;

	    thread = make_proc (inf, threads[i], next_thread_id++);
	    if (last)
	      last->next = thread;
	    else
	      inf->threads = thread;
	    last = thread;
	    proc_debug (thread, "new thread: %d", threads[i]);

	    ptid = gnu_ptid_build (inf->pid, 0, thread->tid);

	    /* Tell GDB's generic thread code.  */

#if 0
	    if (ptid_equal (inferior_ptid, pid_to_ptid (inf->pid)))
	      /* This is the first time we're hearing about thread
		 ids, after a fork-child.  */
	      thread_change_ptid (inferior_ptid, ptid);
	    else if (inf->pending_execs != 0)
	      /* This is a shell thread.  */
	      add_thread_silent (ptid);
	    else
	      add_thread (ptid);
#endif 
	    if (!find_thread_ptid (ptid))
	    {
		    gnu_debug("New thread, pid=%d, tid=%d\n",inf->pid,thread->tid);
		    add_thread (ptid, thread);
		    inferior_ptid = ptid; // need fix!!!!!!!!!!!!!
	    }
	  }
      }

    vm_deallocate (mach_task_self (),
		    (vm_address_t) threads, (num_threads * sizeof (thread_t)));
  }
}
int inf_update_procs (struct inf *inf)
{
	if (!inf->task)
		return 0;
	if (!inf->threads_up_to_date)
		inf_validate_procs (inf);
	return !!inf->task;
}

void inf_set_pid (struct inf *inf, pid_t pid)
{
	task_t task_port;
	struct proc *task = inf->task;

	inf_debug (inf, "setting pid: %d", pid);

	if (pid < 0)
		task_port = MACH_PORT_NULL;
	else
	{
		error_t err = proc_pid2task (proc_server, pid, &task_port);

		if (err){
			error (_("Error getting task for pid %d: %s"),
					pid, "XXXX" );
			/*pid, safe_strerror (err));*/
		}
	}

	inf_debug (inf, "setting task: %d", task_port);

	if (inf->pause_sc)
		task_suspend (task_port);

	if (task && task->port != task_port)
	{
		inf->task = 0;
		inf_validate_procs (inf);	/* Trash all the threads.  */
		_proc_free (task);	/* And the task.  */
	}

	if (task_port != MACH_PORT_NULL)
	{
		inf->task = make_proc (inf, task_port, PROC_TID_TASK);
		inf->threads_up_to_date = 0;
	}

	if (inf->task)
	{
		inf->pid = pid;
		if (inf->pause_sc)
			/* Reflect task_suspend above.  */
			inf->task->sc = inf->task->cur_sc = 1;
	}
	else
		inf->pid = -1;
}
void inf_cleanup (struct inf *inf)
{
	inf_debug (inf, "cleanup");

	inf_clear_wait (inf);

	inf_set_pid (inf, -1);
	inf->pid = 0;
	inf->running = 0;
	inf->stopped = 0;
	inf->nomsg = 1;
	inf->traced = 0;
	inf->no_wait = 0;
	inf->pending_execs = 0;

	if (inf->event_port)
	{
		mach_port_destroy (mach_task_self (), inf->event_port);
		inf->event_port = MACH_PORT_NULL;
	}
}
void inf_startup (struct inf *inf, int pid)
{
	error_t err;

	inf_debug (inf, "startup: pid = %d", pid);

	inf_cleanup (inf);

	/* Make the port on which we receive all events.  */
	err = mach_port_allocate (mach_task_self (),
			MACH_PORT_RIGHT_RECEIVE, &inf->event_port);
	/*if (err)*/
	/*error (_("Error allocating event port: %s"), safe_strerror (err));*/

	/* Make a send right for it, so we can easily copy it for other people.  */
	mach_port_insert_right (mach_task_self (), inf->event_port,
			inf->event_port, MACH_MSG_TYPE_MAKE_SEND);
	inf_set_pid (inf, pid);
}

/* Detachs from INF's inferior task, letting it run once again...  */
void inf_detach (struct inf *inf)
{
  struct proc *task = inf->task;

  inf_debug (inf, "detaching...");

  inf_clear_wait (inf);
  inf_set_step_thread (inf, 0);

  if (task)
    {
      struct proc *thread;

      inf_validate_procinfo (inf);

      inf_set_traced (inf, 0);
      if (inf->stopped)
	{
	  if (inf->nomsg)
	    inf_continue (inf);
	  else
	    inf_signal (inf, GDB_SIGNAL_0);
	}

      proc_restore_exc_port (task);
      task->sc = inf->detach_sc;

      for (thread = inf->threads; thread; thread = thread->next)
	{
	  proc_restore_exc_port (thread);
	  thread->sc = thread->detach_sc;
	}

      inf_update_suspends (inf);
    }

  inf_cleanup (inf);
}
void inf_attach (struct inf *inf, int pid)
{
	inf_debug (inf, "attaching: %d", pid);

	if (inf->pid)
	{
		inf_detach (inf);
	}

	inf_startup (inf, pid);
}

struct inf* make_inf (void)
{
	struct inf *inf = xmalloc (sizeof (struct inf));

	inf->task = 0;
	inf->threads = 0;
	inf->threads_up_to_date = 0;
	inf->pid = 0;
	inf->wait.status.kind = TARGET_WAITKIND_SPURIOUS;
	inf->wait.thread = 0;
	inf->wait.exc.handler = MACH_PORT_NULL;
	inf->wait.exc.reply = MACH_PORT_NULL;
	inf->step_thread = 0;
	inf->signal_thread = 0;
	inf->event_port = MACH_PORT_NULL;
	inf->running = 0;
	inf->stopped = 0;
	inf->nomsg = 1;
	inf->traced = 0;
	inf->no_wait = 0;
	inf->pending_execs = 0;
	inf->pause_sc = 1;
	inf->detach_sc = 0;
	inf->default_thread_run_sc = 0;
	inf->default_thread_pause_sc = 0;
	inf->default_thread_detach_sc = 0;
	inf->want_signals = 1;	/* By default */
	inf->want_exceptions = 1;	/* By default */

	return inf;
}

static struct inf * cur_inf (void)
{
	if (!gnu_current_inf)
		gnu_current_inf = make_inf ();
	return gnu_current_inf;
}


static struct process_info * gnu_add_process (int pid, int attached)
{
	struct process_info *proc;

	proc = add_process (pid, attached);
	proc->tdesc = gnu_tdesc;
	proc->private = xcalloc (1, sizeof (*proc->private));
	proc->private->inf = cur_inf();
	struct inf* inf = gnu_current_inf;

	inf_attach(inf,pid);
	inf->pending_execs = 2;
	inf->nomsg = 1;
	inf->traced = 1;

	inf_resume (inf);

	return proc;
}

static int gnu_create_inferior (char *program, char **allargs)
{
	int pid;
	pid = fork ();
	if (pid < 0)
		perror_with_name ("fork");

	if (pid == 0)
	{
		ptrace (PTRACE_TRACEME);
		setpgid (0, 0);
		execv (program, allargs);

		fprintf (stderr, "Cannot exec %s: %s.\n", program,
				strerror (errno));
		fflush (stderr);
		_exit (0177);
	}

	gnu_add_process(pid,0);
	return pid;
}

/* Fork an inferior process, and start debugging it.  */

/* Set INFERIOR_PID to the first thread available in the child, if any.  */
static int inf_pick_first_thread (void)
{
	if (gnu_current_inf->task && gnu_current_inf->threads)
		/* The first thread.  */
		return gnu_current_inf->threads->tid;
	else
		/* What may be the next thread.  */
		return next_thread_id;
}
static int gnu_attach (unsigned long pid)
{
	return -1; //not support now
	struct inf *inf = cur_inf ();
	/*struct inferior *inferior;*/

	if (pid == getpid ())		/* Trying to masturbate?  */
		error (_("I refuse to debug myself!"));

	inf_debug (inf, "attaching to pid: %d", pid);

	inf_attach (inf, pid);

	inf_update_procs (inf);

	inferior_ptid = gnu_ptid_build (pid, 0, inf_pick_first_thread ());

	inf_validate_procinfo (inf);
	inf->signal_thread = inf->threads ? inf->threads->next : 0;
	inf_set_traced (inf, inf->want_signals);

	gnu_add_process(pid,1);
	add_thread (inferior_ptid, NULL);
	return 0;
}

static int gnu_kill (int pid)
{
	struct proc *task = gnu_current_inf->task;
	struct process_info *process;

	process = find_process_pid (pid);

	if (task)
	{
		proc_debug (task, "terminating...");
		task_terminate (task->port);
		inf_set_pid (gnu_current_inf, -1);
	}
	the_target->mourn(process);
	return 0;
}

static int gnu_detach(int pid)
{
	struct process_info *process;

	process = find_process_pid (pid);
	if (process == NULL)
		return -1;

	inf_detach (gnu_current_inf);

	inferior_ptid = null_ptid;
	the_target->mourn(process);
	return 0;
}


static void gnu_mourn (struct process_info *process)
{
	/* Free our private data.  */
	free (process->private);
	process->private = NULL;

	clear_inferiors ();
}

static void gnu_join(int pid)
{
	/* doesn't need*/
}

static int gnu_thread_alive (ptid_t ptid)
{
	/* this function is copyed from lynx-low.c*/
	return (find_thread_ptid (ptid) != NULL);
}


/* Fill in INF's wait field after a task has died without giving us more
   detailed information.  */
void inf_task_died_status (struct inf *inf)
{
	printf ("Pid %d died with unknown exit status, using SIGKILL.",inf->pid);
	inf->wait.status.kind = TARGET_WAITKIND_SIGNALLED;
	inf->wait.status.value.sig = GDB_SIGNAL_KILL;
}

struct proc * inf_tid_to_thread (struct inf *inf, int tid)
{
	struct proc *thread = inf->threads;

	gnu_debug("[inf_tid_to_thread]:search thread which tid=%d\n",tid);

	while (thread)
		if (thread->tid == tid)
			return thread;
		else
			thread = thread->next;
	return 0;
}
/* Validates INF's stopped, nomsg and traced field from the actual
   proc server state.  Note that the traced field is only updated from
   the proc server state if we do not have a message port.  If we do
   have a message port we'd better look at the tracemask itself.  */
	static void
inf_validate_procinfo (struct inf *inf)
{
	char *noise;
	mach_msg_type_number_t noise_len = 0;
	struct procinfo *pi;
	mach_msg_type_number_t pi_len = 0;
	int info_flags = 0;
	error_t err =
		proc_getprocinfo (proc_server, inf->pid, &info_flags,
				(procinfo_t *) &pi, &pi_len, &noise, &noise_len);

	if (!err)
	{
		inf->stopped = !!(pi->state & PI_STOPPED);
		inf->nomsg = !!(pi->state & PI_NOMSG);
		if (inf->nomsg)
			inf->traced = !!(pi->state & PI_TRACED);
		vm_deallocate (mach_task_self (), (vm_address_t) pi, pi_len);
		if (noise_len > 0)
			vm_deallocate (mach_task_self (), (vm_address_t) noise, noise_len);
	}
}
/* Deliver signal SIG to INF.  If INF is stopped, delivering a signal, even
   signal 0, will continue it.  INF is assumed to be in a paused state, and
   the resume_sc's of INF's threads may be affected.  */
	void
inf_signal (struct inf *inf, enum gdb_signal sig)
{
	error_t err = 0;
	int host_sig = gdb_signal_to_host (sig);

#define NAME gdb_signal_to_name (sig)

	if (host_sig >= _NSIG)
		/* A mach exception.  Exceptions are encoded in the signal space by
		   putting them after _NSIG; this assumes they're positive (and not
		   extremely large)!  */
	{
		struct inf_wait *w = &inf->wait;

		if (w->status.kind == TARGET_WAITKIND_STOPPED
				&& w->status.value.sig == sig
				&& w->thread && !w->thread->aborted)
			/* We're passing through the last exception we received.  This is
			   kind of bogus, because exceptions are per-thread whereas gdb
			   treats signals as per-process.  We just forward the exception to
			   the correct handler, even it's not for the same thread as TID --
			   i.e., we pretend it's global.  */
		{
			struct exc_state *e = &w->exc;

			inf_debug (inf, "passing through exception:"
					" task = %d, thread = %d, exc = %d"
					", code = %d, subcode = %d",
					w->thread->port, inf->task->port,
					e->exception, e->code, e->subcode);
			err =
				exception_raise_request (e->handler,
						e->reply, MACH_MSG_TYPE_MOVE_SEND_ONCE,
						w->thread->port, inf->task->port,
						e->exception, e->code, e->subcode);
		}
		else
			error (_("Can't forward spontaneous exception (%s)."), NAME);
	}
	else
		/* A Unix signal.  */
		if (inf->stopped)
			/* The process is stopped and expecting a signal.  Just send off a
			   request and let it get handled when we resume everything.  */
		{
			inf_debug (inf, "sending %s to stopped process", NAME);
			err =
				INF_MSGPORT_RPC (inf,
						msg_sig_post_untraced_request (msgport,
							inf->event_port,
							MACH_MSG_TYPE_MAKE_SEND_ONCE,
							host_sig, 0,
							refport));
			if (!err)
				/* Posting an untraced signal automatically continues it.
				   We clear this here rather than when we get the reply
				   because we'd rather assume it's not stopped when it
				   actually is, than the reverse.  */
				inf->stopped = 0;
		}
		else
			/* It's not expecting it.  We have to let just the signal thread
			   run, and wait for it to get into a reasonable state before we
			   can continue the rest of the process.  When we finally resume the
			   process the signal we request will be the very first thing that
			   happens.  */
		{
			inf_debug (inf, "sending %s to unstopped process"
					" (so resuming signal thread)", NAME);
			err =
				INF_RESUME_MSGPORT_RPC (inf,
						msg_sig_post_untraced (msgport, host_sig,
							0, refport));
		}

	if (err == EIEIO)
		/* Can't do too much...  */
		warning (_("Can't deliver signal %s: No signal thread."), NAME);
	else if (err)
		warning (_("Delivering signal %s: %s"), NAME, safe_strerror (err));

#undef NAME
}
/* Continue INF without delivering a signal.  This is meant to be used
   when INF does not have a message port.  */
	void
inf_continue (struct inf *inf)
{
	process_t proc;
	error_t err = proc_pid2proc (proc_server, inf->pid, &proc);

	if (!err)
	{
		inf_debug (inf, "continuing process");

		err = proc_mark_cont (proc);
		if (!err)
		{
			struct proc *thread;

			for (thread = inf->threads; thread; thread = thread->next)
				thread_resume (thread->port);

			inf->stopped = 0;
		}
	}

	if (err)
		warning (_("Can't continue process: %s"), safe_strerror (err));
}
/* Returns the number of messages queued for the receive right PORT.  */
	static mach_port_msgcount_t
port_msgs_queued (mach_port_t port)
{
	struct mach_port_status status;
	error_t err =
		mach_port_get_receive_status (mach_task_self (), port, &status);

	if (err)
		return 0;
	else
		return status.mps_msgcount;
}
static void gnu_resume_1 (struct target_ops *ops,
		ptid_t ptid, int step, enum gdb_signal sig)
{
#if 1
	struct proc *step_thread = 0;
	int resume_all;
	struct inf *inf = gnu_current_inf;

	inf_debug (inf, "ptid = %s, step = %d, sig = %d",
			target_pid_to_str (ptid), step, sig);

	inf_validate_procinfo (inf);

	if (sig != GDB_SIGNAL_0 || inf->stopped)
	{
		if (sig == GDB_SIGNAL_0 && inf->nomsg)
			inf_continue (inf);
		else
			inf_signal (inf, sig);
	}
	else if (inf->wait.exc.reply != MACH_PORT_NULL)
		/* We received an exception to which we have chosen not to forward, so
		   abort the faulting thread, which will perhaps retake it.  */
	{
		proc_abort (inf->wait.thread, 1);
		/*warning (_("Aborting %s with unforwarded exception %s."),*/
		/*proc_string (inf->wait.thread),*/
		/*gdb_signal_to_name (inf->wait.status.value.sig));*/
	}

	if (port_msgs_queued (inf->event_port))
		/* If there are still messages in our event queue, don't bother resuming
		   the process, as we're just going to stop it right away anyway.  */
		return;

	inf_update_procs (inf);

	/* A specific PTID means `step only this process id'.  */
	resume_all = ptid_equal (ptid, minus_one_ptid);

	if (resume_all)
		/* Allow all threads to run, except perhaps single-stepping one.  */
	{
		inf_debug (inf, "running all threads; tid = %d", PIDGET (inferior_ptid));
		ptid = inferior_ptid;	/* What to step.  */
		inf_set_threads_resume_sc (inf, 0, 1);
	}
	else
		/* Just allow a single thread to run.  */
	{
		struct proc *thread = inf_tid_to_thread (inf, gnu_get_tid (ptid));

		if (!thread)
			error (_("Can't run single thread id %s: no such thread!"),
					target_pid_to_str (ptid));
		inf_debug (inf, "running one thread: %s", target_pid_to_str (ptid));
		inf_set_threads_resume_sc (inf, thread, 0);
	}

	if (step)
	{
		step_thread = inf_tid_to_thread (inf, gnu_get_tid (ptid));
		if (!step_thread)
			warning (_("Can't step thread id %s: no such thread."),
					target_pid_to_str (ptid));
		else
			inf_debug (inf, "stepping thread: %s", target_pid_to_str (ptid));
	}
	if (step_thread != inf->step_thread)
		inf_set_step_thread (inf, step_thread);

	inf_debug (inf, "here we go...");
	inf_resume (inf);
#endif
}

static void gnu_resume (struct thread_resume *resume_info, size_t n)
{
	/* FIXME: Assume for now that n == 1.  */
	ptid_t ptid = resume_info[0].thread;
	const int step = (resume_info[0].kind == resume_step
			? 1 : 0); //1 means step, 0 means contiune
	const int signal = resume_info[0].sig;
	if (ptid_equal (ptid, minus_one_ptid))
		ptid = thread_to_gdb_id (current_inferior);

	regcache_invalidate ();

	gnu_debug("in gnu_resume: ptid=%d, step=%d, signal=%d\n",ptid,step,signal);

	/*my_resume();*/
	/*static void gnu_resume_1 (struct target_ops *ops,ptid_t ptid, int step, enum gdb_signal sig)*/
	gnu_resume_1(NULL,ptid,step,signal);

}

void inf_suspend (struct inf *inf)
{
	struct proc *thread;

	inf_update_procs (inf);

	for (thread = inf->threads; thread; thread = thread->next)
		thread->sc = thread->pause_sc;

	if (inf->task)
		inf->task->sc = inf->pause_sc;

	inf_update_suspends (inf);
}

static ptid_t gnu_wait_1 (ptid_t ptid,
		struct target_waitstatus *status, int target_options)
{
	struct msg
	{
		mach_msg_header_t hdr;
		mach_msg_type_t type;
		int data[8000];
	} msg;
	error_t err;
	struct proc *thread;
	struct inf *inf = gnu_current_inf;

	extern int exc_server (mach_msg_header_t *, mach_msg_header_t *);
	extern int msg_reply_server (mach_msg_header_t *, mach_msg_header_t *);
	extern int notify_server (mach_msg_header_t *, mach_msg_header_t *);
	extern int process_reply_server (mach_msg_header_t *, mach_msg_header_t *);

	gdb_assert (inf->task);

	if (!inf->threads && !inf->pending_execs)
		/* No threads!  Assume that maybe some outside agency is frobbing our
		   task, and really look for new threads.  If we can't find any, just tell
		   the user to try again later.  */
	{
		inf_validate_procs (inf);
		if (!inf->threads && !inf->task->dead)
			error (_("There are no threads; try again later."));
	}

	waiting_inf = inf;

	inf_debug (inf, "waiting for: %s", target_pid_to_str (ptid));

rewait:
	if (proc_wait_pid != inf->pid && !inf->no_wait)
		/* Always get information on events from the proc server.  */
	{
		inf_debug (inf, "requesting wait on pid %d", inf->pid);

		if (proc_wait_pid)
			/* The proc server is single-threaded, and only allows a single
			   outstanding wait request, so we have to cancel the previous one.  */
		{
			inf_debug (inf, "cancelling previous wait on pid %d", proc_wait_pid);
			interrupt_operation (proc_server, 0);
		}

		err =
			proc_wait_request (proc_server, inf->event_port, inf->pid, WUNTRACED);
		if (err)
			warning (_("wait request failed: %s"), safe_strerror (err));
		else
		{
			inf_debug (inf, "waits pending: %d", proc_waits_pending);
			proc_wait_pid = inf->pid;
			/* Even if proc_waits_pending was > 0 before, we still won't
			   get any other replies, because it was either from a
			   different INF, or a different process attached to INF --
			   and the event port, which is the wait reply port, changes
			   when you switch processes.  */
			proc_waits_pending = 1;
		}
	}

	inf_clear_wait (inf);

	/* What can happen? (1) Dead name notification; (2) Exceptions arrive;
	   (3) wait reply from the proc server.  */

	inf_debug (inf, "waiting for an event...");
	err = mach_msg (&msg.hdr, MACH_RCV_MSG | MACH_RCV_INTERRUPT,
			0, sizeof (struct msg), inf->event_port,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

	/* Re-suspend the task.  */
	inf_suspend (inf);

	if (!inf->task && inf->pending_execs)
		/* When doing an exec, it's possible that the old task wasn't reused
		   (e.g., setuid execs).  So if the task seems to have disappeared,
		   attempt to refetch it, as the pid should still be the same.  */
		inf_set_pid (inf, inf->pid);

	if (err == EMACH_RCV_INTERRUPTED)
		printf("interrupted\n");
	/*inf_debug (inf, "interrupted");*/
	else if (err)
		printf ("Couldn't wait for an event:\n");
	/*error (_("Couldn't wait for an event: %s"), safe_strerror (err));*/
	else
	{
		struct
		{
			mach_msg_header_t hdr;
			mach_msg_type_t err_type;
			kern_return_t err;
			char noise[200];
		}
		reply;

		inf_debug (inf, "event: msgid = %d", msg.hdr.msgh_id);

		/* Handle what we got.  */
		if (!notify_server (&msg.hdr, &reply.hdr)
				&& !exc_server (&msg.hdr, &reply.hdr)
				&& !process_reply_server (&msg.hdr, &reply.hdr)
				&& !msg_reply_server (&msg.hdr, &reply.hdr))
			/* Whatever it is, it's something strange.  */
			error (_("Got a strange event, msg id = %d."), msg.hdr.msgh_id);

		if (reply.err)
			error (_("Handling event, msgid = %d: %s"),
					msg.hdr.msgh_id, safe_strerror (reply.err));
	}

	if (inf->pending_execs)
		/* We're waiting for the inferior to finish execing.  */
	{
		struct inf_wait *w = &inf->wait;
		enum target_waitkind kind = w->status.kind;

		if (kind == TARGET_WAITKIND_SPURIOUS)
			/* Since gdb is actually counting the number of times the inferior
			   stops, expecting one stop per exec, we only return major events
			   while execing.  */
		{
			w->suppress = 1;
			inf_debug (inf, "pending_execs = %d, ignoring minor event",
					inf->pending_execs);
		}
		else if (kind == TARGET_WAITKIND_STOPPED
				&& w->status.value.sig == GDB_SIGNAL_TRAP)
			/* Ah hah!  A SIGTRAP from the inferior while starting up probably
			   means we've succesfully completed an exec!  */
		{
			if (--inf->pending_execs == 0)
				/* We're done!  */
			{
#if 0				/* do we need this?  */
				prune_threads (1);	/* Get rid of the old shell
							   threads.  */
				renumber_threads (0);	/* Give our threads reasonable
							   names.  */
#endif
			}
			inf_debug (inf, "pending exec completed, pending_execs => %d",
					inf->pending_execs);
		}
		else if (kind == TARGET_WAITKIND_STOPPED)
			/* It's possible that this signal is because of a crashed process
			   being handled by the hurd crash server; in this case, the process
			   will have an extra task suspend, which we need to know about.
			   Since the code in inf_resume that normally checks for this is
			   disabled while INF->pending_execs, we do the check here instead.  */
			inf_validate_task_sc (inf);
	}

	if (inf->wait.suppress)
		/* Some totally spurious event happened that we don't consider
		   worth returning to gdb.  Just keep waiting.  */
	{
		inf_debug (inf, "suppressing return, rewaiting...");
		inf_resume (inf);
		goto rewait;
	}

	/* Pass back out our results.  */
	memcpy (status, &inf->wait.status, sizeof (*status));

	thread = inf->wait.thread;
	if (thread)
		ptid = gnu_ptid_build (inf->pid, 0, thread->tid);
	else if (ptid_equal (ptid, minus_one_ptid))
		thread = inf_tid_to_thread (inf, -1);
	else
		thread = inf_tid_to_thread (inf, gnu_get_tid (ptid));

	if (!thread || thread->port == MACH_PORT_NULL)
	{
		/* TID is dead; try and find a new thread.  */
		if (inf_update_procs (inf) && inf->threads)
			ptid = gnu_ptid_build (inf->pid, 0, inf->threads->tid);
		/* The first
		   available
		   thread.  */
		else
			ptid = inferior_ptid;	/* let wait_for_inferior handle exit case */
	}

	if (thread
			&& !ptid_equal (ptid, minus_one_ptid)
			&& status->kind != TARGET_WAITKIND_SPURIOUS
			&& inf->pause_sc == 0 && thread->pause_sc == 0)
		/* If something actually happened to THREAD, make sure we
		   suspend it.  */
	{
		thread->sc = 1;
		inf_update_suspends (inf);
	}

	inf_debug (inf, "returning ptid = %s, status = %s (%d)",
			target_pid_to_str (ptid),
			status->kind == TARGET_WAITKIND_EXITED ? "EXITED"
			: status->kind == TARGET_WAITKIND_STOPPED ? "STOPPED"
			: status->kind == TARGET_WAITKIND_SIGNALLED ? "SIGNALLED"
			: status->kind == TARGET_WAITKIND_LOADED ? "LOADED"
			: status->kind == TARGET_WAITKIND_SPURIOUS ? "SPURIOUS"
			: "?",
			status->value.integer);

	inferior_ptid=ptid;
	return ptid;
}

static ptid_t gnu_wait (ptid_t ptid,
		struct target_waitstatus *status, int target_options)
{
	ptid_t event_ptid;
	gnu_debug("gnu_wait: [%s]", target_pid_to_str (ptid));
	event_ptid = gnu_wait_1 (ptid, status, target_options);
	gnu_debug ("          -> (status->kind = %d)\n",status->kind);
	return event_ptid;
}

#define I386_NUM_GREGS	16

//copy from i386gnu-nat.c
/* Offset to the thread_state_t location where REG is stored.  */
#define REG_OFFSET(reg) offsetof (struct i386_thread_state, reg)

/* At REG_OFFSET[N] is the offset to the thread_state_t location where
   the GDB register N is stored.  */
static int reg_offset[] =
{
	REG_OFFSET (eax), REG_OFFSET (ecx), REG_OFFSET (edx), REG_OFFSET (ebx),
	REG_OFFSET (uesp), REG_OFFSET (ebp), REG_OFFSET (esi), REG_OFFSET (edi),
	REG_OFFSET (eip), REG_OFFSET (efl), REG_OFFSET (cs), REG_OFFSET (ss),
	REG_OFFSET (ds), REG_OFFSET (es), REG_OFFSET (fs), REG_OFFSET (gs)
};

/* Offset to the greg_t location where REG is stored.  */
#define CREG_OFFSET(reg) (REG_##reg * 4)

/* At CREG_OFFSET[N] is the offset to the greg_t location where
   the GDB register N is stored.  */
static int creg_offset[] =
{
	CREG_OFFSET (EAX), CREG_OFFSET (ECX), CREG_OFFSET (EDX), CREG_OFFSET (EBX),
	CREG_OFFSET (UESP), CREG_OFFSET (EBP), CREG_OFFSET (ESI), CREG_OFFSET (EDI),
	CREG_OFFSET (EIP), CREG_OFFSET (EFL), CREG_OFFSET (CS), CREG_OFFSET (SS),
	CREG_OFFSET (DS), CREG_OFFSET (ES), CREG_OFFSET (FS), CREG_OFFSET (GS)
};
#define REG_ADDR(state, regnum) ((char *)(state) + reg_offset[regnum])
#define CREG_ADDR(state, regnum) ((const char *)(state) + creg_offset[regnum])

/* Return printable description of proc.  */
	char *
proc_string (struct proc *proc)
{
	static char tid_str[80];

	if (proc_is_task (proc))
		xsnprintf (tid_str, sizeof (tid_str), "process %d", proc->inf->pid);
	else
		xsnprintf (tid_str, sizeof (tid_str), "Thread %d.%d",
				proc->inf->pid, proc->tid);
	return tid_str;
}

	static void
fetch_fpregs (struct regcache *regcache, struct proc *thread)
{
	gnu_debug("fetch_fpregs() not support now\n");
}
	static void
store_fpregs (const struct regcache *regcache, struct proc *thread, int regno)
{

	gnu_debug("store_fpregs() not support now\n");
}
/* Fetch register REGNO, or all regs if REGNO is -1.  */
	static void
gnu_fetch_registers_1 (struct target_ops *ops,
		struct regcache *regcache, int regno)
{
#if 1
	struct proc *thread;

	/* Make sure we know about new threads.  */
	inf_update_procs (gnu_current_inf);

	thread = inf_tid_to_thread (gnu_current_inf,
			gnu_get_tid (inferior_ptid));
	if (!thread)
		error (_("[gnu_fetch_registers_1]Can't fetch registers from thread %s: No such thread"),
				target_pid_to_str (inferior_ptid));

	if (regno < I386_NUM_GREGS || regno == -1)
	{
		thread_state_t state;

		/* This does the dirty work for us.  */
		state = proc_get_state (thread, 0);
		if (!state)
		{
			warning (_("Couldn't fetch registers from %s"),
					proc_string (thread));

			return;
		}

		if (regno == -1)
		{
			int i;

			proc_debug (thread, "fetching all register");

			for (i = 0; i < I386_NUM_GREGS; i++)
				/*regcache_raw_supply (regcache, i, REG_ADDR (state, i));*/
				supply_register (regcache, i, REG_ADDR(state,i));
			thread->fetched_regs = ~0;
		}
		else
		{
			/*proc_debug (thread, "fetching register %s",*/
			/*gdbarch_register_name (get_regcache_arch (regcache),*/
			/*regno));*/

			/*regcache_raw_supply (regcache, regno,REG_ADDR (state, regno));*/
			supply_register (regcache, regno, REG_ADDR(state,regno));
			thread->fetched_regs |= (1 << regno);
		}
	}

	if (regno >= I386_NUM_GREGS || regno == -1)
	{
		proc_debug (thread, "fetching floating-point registers");

		fetch_fpregs (regcache, thread);
	}
#endif
}
void gnu_fetch_registers (struct regcache *regcache, int regno)
{
	gnu_debug("gnu_fetch_registers() regno=%d\n",regno);
	return gnu_fetch_registers_1(NULL,regcache,regno);
}

/* Store at least register REGNO, or all regs if REGNO == -1.  */
//copy from i386gnu-nat.c
	static void
gnu_store_registers_1 (struct target_ops *ops,
		struct regcache *regcache, int regno)
{
#if 1
	struct proc *thread;
	/*struct gdbarch *gdbarch = get_regcache_arch (regcache);*/
	const struct target_desc *gdbarch = regcache->tdesc;

	/* Make sure we know about new threads.  */
	inf_update_procs (gnu_current_inf);

	thread = inf_tid_to_thread (gnu_current_inf,
			gnu_get_tid (inferior_ptid));
	if (!thread)
		error (_("Couldn't store registers into thread %s: No such thread"),
				target_pid_to_str (inferior_ptid));

	if (regno < I386_NUM_GREGS || regno == -1)
	{
		thread_state_t state;
		thread_state_data_t old_state;
		int was_aborted = thread->aborted;
		int was_valid = thread->state_valid;
		int trace;

		if (!was_aborted && was_valid)
			memcpy (&old_state, &thread->state, sizeof (old_state));

		state = proc_get_state (thread, 1);
		if (!state)
		{
			warning (_("Couldn't store registers into %s"),
					proc_string (thread));
			return;
		}

		/* Save the T bit.  We might try to restore the %eflags register
		   below, but changing the T bit would seriously confuse GDB.  */
		trace = ((struct i386_thread_state *)state)->efl & 0x100;

		if (!was_aborted && was_valid)
			/* See which registers have changed after aborting the thread.  */
		{
			int check_regno;

			for (check_regno = 0; check_regno < I386_NUM_GREGS; check_regno++)
				if ((thread->fetched_regs & (1 << check_regno))
						&& memcpy (REG_ADDR (&old_state, check_regno),
							REG_ADDR (state, check_regno),
							register_size (gdbarch, check_regno)))
					/* Register CHECK_REGNO has changed!  Ack!  */
				{
					/*warning (_("Register %s changed after the thread was aborted"),*/
					/*gdbarch_register_name (gdbarch, check_regno));*/
					if (regno >= 0 && regno != check_regno)
						/* Update GDB's copy of the register.  */
						/*regcache_raw_supply (regcache, check_regno,REG_ADDR (state, check_regno));*/
						supply_register (regcache, check_regno, REG_ADDR(state,check_regno));
					else
						warning (_("... also writing this register!  "
									"Suspicious..."));
				}
		}

		if (regno == -1)
		{
			int i;

			proc_debug (thread, "storing all registers");

			for (i = 0; i < I386_NUM_GREGS; i++)
				/*if (REG_VALID == regcache_register_status (regcache, i))*/
				/*regcache_raw_collect (regcache, i, REG_ADDR (state, i));*/
				collect_register (regcache, i, REG_ADDR (state, i));
		}
		else
		{
			/*proc_debug (thread, "storing register %s",gdbarch_register_name (gdbarch, regno));*/

			/*gdb_assert (REG_VALID == regcache_register_status (regcache, regno));*/
			/*regcache_raw_collect (regcache, regno, REG_ADDR (state, regno));*/
			collect_register (regcache, regno, REG_ADDR (state, regno));
		}

		/* Restore the T bit.  */
		((struct i386_thread_state *)state)->efl &= ~0x100;
		((struct i386_thread_state *)state)->efl |= trace;
	}

	if (regno >= I386_NUM_GREGS || regno == -1)
	{
		proc_debug (thread, "storing floating-point registers");

		store_fpregs (regcache, thread, regno);
	}
#endif
}
void gnu_store_registers (struct regcache *regcache, int regno)
{
	gnu_debug("gnu_store_registers() regno=%d\n",regno);
	return gnu_store_registers_1(NULL,regcache,regno);
}

/* Read inferior task's LEN bytes from ADDR and copy it to MYADDR in
   gdb's address space.  Return 0 on failure; number of bytes read
   otherwise.  */
	int
gnu_read_inferior (task_t task, CORE_ADDR addr,unsigned char *myaddr, int length)
{
	error_t err;
	vm_address_t low_address = (vm_address_t) trunc_page (addr);
	vm_size_t aligned_length =
		(vm_size_t) round_page (addr + length) - low_address;
	pointer_t copied;
	int copy_count;

	/* Get memory from inferior with page aligned addresses.  */
	err = vm_read (task, low_address, aligned_length, &copied, &copy_count);
	if (err)
		return 0;

	err = hurd_safe_copyin (myaddr, (void *) (addr - low_address + copied),
			length);
	if (err)
	{
		warning (_("Read from inferior faulted: %s"), safe_strerror (err));
		length = 0;
	}

	err = vm_deallocate (mach_task_self (), copied, copy_count);
	if (err)
		warning (_("gnu_read_inferior vm_deallocate failed: %s"),
				safe_strerror (err));

	return length;
}

#define CHK_GOTO_OUT(str,ret) \
	do if (ret != KERN_SUCCESS) { errstr = #str; goto out; } while(0)

struct vm_region_list
{
	struct vm_region_list *next;
	vm_prot_t protection;
	vm_address_t start;
	vm_size_t length;
};
/*struct obstack region_obstack;*/

/* Write gdb's LEN bytes from MYADDR and copy it to ADDR in inferior
   task's address space.  */
int gnu_write_inferior (task_t task, CORE_ADDR addr, const char *myaddr, int length)
{
	error_t err = 0;
	vm_address_t low_address = (vm_address_t) trunc_page (addr);
	vm_size_t aligned_length =
		(vm_size_t) round_page (addr + length) - low_address;
	pointer_t copied;
	int copy_count;
	int deallocate = 0;

	char *errstr = "Bug in gnu_write_inferior";

	struct vm_region_list *region_element;
	struct vm_region_list *region_head = (struct vm_region_list *) NULL;

	/* Get memory from inferior with page aligned addresses.  */
	err = vm_read (task,
			low_address,
			aligned_length,
			&copied,
			&copy_count);
	CHK_GOTO_OUT ("gnu_write_inferior vm_read failed", err);

	deallocate++;

	err = hurd_safe_copyout ((void *) (addr - low_address + copied),
			myaddr, length);
	CHK_GOTO_OUT ("Write to inferior faulted", err);

	/*obstack_init (&region_obstack);*/

	/* Do writes atomically.
	   First check for holes and unwritable memory.  */
	{
		vm_size_t remaining_length = aligned_length;
		vm_address_t region_address = low_address;

		struct vm_region_list *scan;

		while (region_address < low_address + aligned_length)
		{
			vm_prot_t protection;
			vm_prot_t max_protection;
			vm_inherit_t inheritance;
			boolean_t shared;
			mach_port_t object_name;
			vm_offset_t offset;
			vm_size_t region_length = remaining_length;
			vm_address_t old_address = region_address;

			err = vm_region (task,
					&region_address,
					&region_length,
					&protection,
					&max_protection,
					&inheritance,
					&shared,
					&object_name,
					&offset);
			CHK_GOTO_OUT ("vm_region failed", err);

			/* Check for holes in memory.  */
			if (old_address != region_address)
			{
				warning (_("No memory at 0x%x. Nothing written"),
						old_address);
				err = KERN_SUCCESS;
				length = 0;
				goto out;
			}

			if (!(max_protection & VM_PROT_WRITE))
			{
				warning (_("Memory at address 0x%x is unwritable. "
							"Nothing written"),
						old_address);
				err = KERN_SUCCESS;
				length = 0;
				goto out;
			}

			/* Chain the regions for later use.  */
			/*region_element =*/
			/*(struct vm_region_list *)*/
			/*obstack_alloc (&region_obstack, sizeof (struct vm_region_list));*/
			region_element =
				(struct vm_region_list *)
				malloc(sizeof (struct vm_region_list));

			region_element->protection = protection;
			region_element->start = region_address;
			region_element->length = region_length;

			/* Chain the regions along with protections.  */
			region_element->next = region_head;
			region_head = region_element;

			region_address += region_length;
			remaining_length = remaining_length - region_length;
		}

		/* If things fail after this, we give up.
		   Somebody is messing up inferior_task's mappings.  */

		/* Enable writes to the chained vm regions.  */
		for (scan = region_head; scan; scan = scan->next)
		{
			if (!(scan->protection & VM_PROT_WRITE))
			{
				err = vm_protect (task,
						scan->start,
						scan->length,
						FALSE,
						scan->protection | VM_PROT_WRITE);
				CHK_GOTO_OUT ("vm_protect: enable write failed", err);
			}
		}

		err = vm_write (task,
				low_address,
				copied,
				aligned_length);
		CHK_GOTO_OUT ("vm_write failed", err);

		/* Set up the original region protections, if they were changed.  */
		for (scan = region_head; scan; scan = scan->next)
		{
			if (!(scan->protection & VM_PROT_WRITE))
			{
				err = vm_protect (task,
						scan->start,
						scan->length,
						FALSE,
						scan->protection);
				CHK_GOTO_OUT ("vm_protect: enable write failed", err);
			}
		}
	}

out:
	if (deallocate)
	{
		/*obstack_free (&region_obstack, 0);*/

		(void) vm_deallocate (mach_task_self (),
				copied,
				copy_count);
	}

	if (err != KERN_SUCCESS)
	{
		warning (_("%s: %s"), errstr, mach_error_string (err));
		return 0;
	}

	return length;
}
static int gnu_read_memory (CORE_ADDR addr, unsigned char *myaddr, int length)
{
	int ret=0;
	task_t task = (gnu_current_inf
			? (gnu_current_inf->task
				? gnu_current_inf->task->port : 0)
			: 0);
	if (task == MACH_PORT_NULL)
		return 0;
	ret = gnu_read_inferior(task,addr,myaddr,length);
	if(length!=ret){
		gnu_debug("gnu_read_inferior,length=%d, but return %d\n",length,ret);
		return -1;
	}
	return 0;
}

static int gnu_write_memory (CORE_ADDR addr, const unsigned char *myaddr, int length)
{
	int ret=0;
	task_t task = (gnu_current_inf
			? (gnu_current_inf->task
				? gnu_current_inf->task->port : 0)
			: 0);
	if (task == MACH_PORT_NULL)
		return 0;
	ret = gnu_write_inferior(task,addr,myaddr,length);
	if(length!=ret){
		gnu_debug("gnu_write_inferior,length=%d, but return %d\n",length,ret);
		return -1;
	}
	return 0;
}

static void gnu_request_interrupt (void)
{
	printf("gnu_request_interrupt not support!\n");
	exit(-1);
}

/* Helper function for child_wait and the derivatives of child_wait.
   HOSTSTATUS is the waitstatus from wait() or the equivalent; store our
   translation of that in OURSTATUS.  */
void store_waitstatus (struct target_waitstatus *ourstatus, int hoststatus)
{
	if (WIFEXITED (hoststatus))
	{
		ourstatus->kind = TARGET_WAITKIND_EXITED;
		ourstatus->value.integer = WEXITSTATUS (hoststatus);
	}
	else if (!WIFSTOPPED (hoststatus))
	{
		ourstatus->kind = TARGET_WAITKIND_SIGNALLED;
		ourstatus->value.sig = gdb_signal_from_host (WTERMSIG (hoststatus));
	}
	else
	{
		ourstatus->kind = TARGET_WAITKIND_STOPPED;
		ourstatus->value.sig = gdb_signal_from_host (WSTOPSIG (hoststatus));
	}
}
/* Validates INF's task suspend count.  If it's higher than we expect,
   verify with the user before `stealing' the extra count.  */
static void inf_validate_task_sc (struct inf *inf)
{
	char *noise;
	mach_msg_type_number_t noise_len = 0;
	struct procinfo *pi;
	mach_msg_type_number_t pi_len = 0;
	int info_flags = PI_FETCH_TASKINFO;
	int suspend_count = -1;
	error_t err;

retry:
	err = proc_getprocinfo (proc_server, inf->pid, &info_flags,
			(procinfo_t *) &pi, &pi_len, &noise, &noise_len);
	if (err)
	{
		inf->task->dead = 1; /* oh well */
		return;
	}

	if (inf->task->cur_sc < pi->taskinfo.suspend_count && suspend_count == -1)
	{
		/* The proc server might have suspended the task while stopping
		   it.  This happens when the task is handling a traced signal.
		   Refetch the suspend count.  The proc server should be
		   finished stopping the task by now.  */
		suspend_count = pi->taskinfo.suspend_count;
		goto retry;
	}

	suspend_count = pi->taskinfo.suspend_count;

	vm_deallocate (mach_task_self (), (vm_address_t) pi, pi_len);
	if (noise_len > 0)
		vm_deallocate (mach_task_self (), (vm_address_t) pi, pi_len);

	if (inf->task->cur_sc < suspend_count)
	{
#if 0
		int abort;

		target_terminal_ours ();	/* Allow I/O.  */
		abort = !query (_("Pid %d has an additional task suspend count of %d;"
					" clear it? "), inf->pid,
				suspend_count - inf->task->cur_sc);
		target_terminal_inferior ();	/* Give it back to the child.  */

		if (abort)
			error (_("Additional task suspend count left untouched."));
#endif

		//need fix!
		inf->task->cur_sc = suspend_count;
	}
}

void inf_resume (struct inf *inf)
{
	struct proc *thread;

	inf_update_procs (inf);

	for (thread = inf->threads; thread; thread = thread->next)
		thread->sc = thread->resume_sc;

	if (inf->task)
	{
		if (!inf->pending_execs)
			/* Try to make sure our task count is correct -- in the case where
			   we're waiting for an exec though, things are too volatile, so just
			   assume things will be reasonable (which they usually will be).  */
			inf_validate_task_sc (inf);
		inf->task->sc = 0;
	}

	inf_update_suspends (inf);
}
/* Converts a thread port to a struct proc.  */
struct proc * inf_port_to_thread (struct inf *inf, mach_port_t port)
{
	struct proc *thread = inf->threads;

	while (thread)
		if (thread->port == port)
			return thread;
		else
			thread = thread->next;
	return 0;
}
error_t S_exception_raise_request (mach_port_t port, mach_port_t reply_port,
		thread_t thread_port, task_t task_port,
		int exception, int code, int subcode)
{
#if 1
	struct inf *inf = waiting_inf;
	struct proc *thread = inf_port_to_thread (inf, thread_port);

	inf_debug(waiting_inf,
			"S_exception_raise_request thread = %d, task = %d, exc = %d, code = %d, subcode = %d",
			thread_port, task_port, exception, code, subcode);

	if (!thread)
		/* We don't know about thread?  */
	{
		inf_update_procs (inf);
		thread = inf_port_to_thread (inf, thread_port);
		if (!thread)
			/* Give up, the generating thread is gone.  */
			return 0;
	}

	mach_port_deallocate (mach_task_self (), thread_port);
	mach_port_deallocate (mach_task_self (), task_port);

	if (!thread->aborted)
		/* THREAD hasn't been aborted since this exception happened (abortion
		   clears any exception state), so it must be real.  */
	{
		/* Store away the details; this will destroy any previous info.  */
		inf->wait.thread = thread;

		inf->wait.status.kind = TARGET_WAITKIND_STOPPED;

		if (exception == EXC_BREAKPOINT)
			/* GDB likes to get SIGTRAP for breakpoints.  */
		{
			inf->wait.status.value.sig = GDB_SIGNAL_TRAP;
			mach_port_deallocate (mach_task_self (), reply_port);
		}
		else
			/* Record the exception so that we can forward it later.  */
		{
			if (thread->exc_port == port)
			{
				inf_debug (waiting_inf, "Handler is thread exception port <%d>",
						thread->saved_exc_port);
				inf->wait.exc.handler = thread->saved_exc_port;
			}
			else
			{
				inf_debug (waiting_inf, "Handler is task exception port <%d>",
						inf->task->saved_exc_port);
				inf->wait.exc.handler = inf->task->saved_exc_port;
				gdb_assert (inf->task->exc_port == port);
			}
			if (inf->wait.exc.handler != MACH_PORT_NULL)
				/* Add a reference to the exception handler.  */
				mach_port_mod_refs (mach_task_self (),
						inf->wait.exc.handler, MACH_PORT_RIGHT_SEND,
						1);

			inf->wait.exc.exception = exception;
			inf->wait.exc.code = code;
			inf->wait.exc.subcode = subcode;
			inf->wait.exc.reply = reply_port;

			/* Exceptions are encoded in the signal space by putting
			   them after _NSIG; this assumes they're positive (and not
			   extremely large)!  */
			inf->wait.status.value.sig =
				gdb_signal_from_host (_NSIG + exception);
		}
	}
	else
		/* A supppressed exception, which ignore.  */
	{
		inf->wait.suppress = 1;
		mach_port_deallocate (mach_task_self (), reply_port);
	}

#endif
	return 0;
}
	error_t
S_proc_wait_reply (mach_port_t reply, error_t err,
		int status, int sigcode, rusage_t rusage, pid_t pid)
{
	struct inf *inf = waiting_inf;

	inf_debug(inf, "S_proc_wait_reply  err = %s, pid = %d, status = 0x%x, sigcode = %d",
			err ? safe_strerror (err) : "0", pid, status, sigcode);

	if (err && proc_wait_pid && (!inf->task || !inf->task->port))
		/* Ack.  The task has died, but the task-died notification code didn't
		   tell anyone because it thought a more detailed reply from the
		   procserver was forthcoming.  However, we now learn that won't
		   happen...  So we have to act like the task just died, and this time,
		   tell the world.  */
		inf_task_died_status (inf);

	if (--proc_waits_pending == 0)
		/* PROC_WAIT_PID represents the most recent wait.  We will always get
		   replies in order because the proc server is single threaded.  */
		proc_wait_pid = 0;

	inf_debug (inf, "waits pending now: %d", proc_waits_pending);

	if (err)
	{
		if (err != EINTR)
		{
			/*warning (_("Can't wait for pid %d: %s"),*/
			/*inf->pid, safe_strerror (err));*/
			inf->no_wait = 1;

			/* Since we can't see the inferior's signals, don't trap them.  */
			inf_set_traced (inf, 0);
		}
	}
	else if (pid == inf->pid)
	{
		store_waitstatus (&inf->wait.status, status);
		if (inf->wait.status.kind == TARGET_WAITKIND_STOPPED)
			/* The process has sent us a signal, and stopped itself in a sane
			   state pending our actions.  */
		{
			inf_debug (inf, "process has stopped itself");
			inf->stopped = 1;
		}
	}
	else
		inf->wait.suppress = 1;	/* Something odd happened.  Ignore.  */

	return 0;
}
	error_t
S_msg_sig_post_untraced_reply (mach_port_t reply, error_t err)
{
#if 1
	struct inf *inf = waiting_inf;

	if (err == EBUSY)
		/* EBUSY is what we get when the crash server has grabbed control of the
		   process and doesn't like what signal we tried to send it.  Just act
		   like the process stopped (using a signal of 0 should mean that the
		 *next* time the user continues, it will pass signal 0, which the crash
		 server should like).  */
	{
		inf->wait.status.kind = TARGET_WAITKIND_STOPPED;
		inf->wait.status.value.sig = GDB_SIGNAL_0;
	}
	else if (err)
		warning (_("Signal delivery failed: %s"), safe_strerror (err));

	if (err)
		/* We only get this reply when we've posted a signal to a process which we
		   thought was stopped, and which we expected to continue after the signal.
		   Given that the signal has failed for some reason, it's reasonable to
		   assume it's still stopped.  */
		inf->stopped = 1;
	else
		inf->wait.suppress = 1;
#endif
	return 0;
}
	error_t
S_msg_sig_post_reply (mach_port_t reply, error_t err)
{
	printf("bug in S_msg_sig_post_reply!!\n");
	exit(-238);
}
error_t do_mach_notify_dead_name (mach_port_t notify, mach_port_t dead_port)
{
	struct inf *inf = waiting_inf;

	inf_debug (waiting_inf, "port = %d", dead_port);

	if (inf->task && inf->task->port == dead_port)
	{
		proc_debug (inf->task, "is dead");
		inf->task->port = MACH_PORT_NULL;
		if (proc_wait_pid == inf->pid)
			/* We have a wait outstanding on the process, which will return more
			   detailed information, so delay until we get that.  */
			inf->wait.suppress = 1;
		else
			/* We never waited for the process (maybe it wasn't a child), so just
			   pretend it got a SIGKILL.  */
			inf_task_died_status (inf);
	}
	else
	{
		struct proc *thread = inf_port_to_thread (inf, dead_port);

		if (thread)
		{
			proc_debug (thread, "is dead");
			thread->port = MACH_PORT_NULL;
		}

		if (inf->task->dead)
			/* Since the task is dead, its threads are dying with it.  */
			inf->wait.suppress = 1;
	}

	mach_port_deallocate (mach_task_self (), dead_port);
	inf->threads_up_to_date = 0;	/* Just in case.  */

	return 0;
}
static error_t ill_rpc (char *fun)
{
  warning (_("illegal rpc: %s"), fun);
  return 0;
}
	error_t
do_mach_notify_no_senders (mach_port_t notify, mach_port_mscount_t count)
{
	return ill_rpc ("do_mach_notify_no_senders");
}

	error_t
do_mach_notify_port_deleted (mach_port_t notify, mach_port_t name)
{
	return ill_rpc ("do_mach_notify_port_deleted");
}

	error_t
do_mach_notify_msg_accepted (mach_port_t notify, mach_port_t name)
{
	return ill_rpc ("do_mach_notify_msg_accepted");
}

	error_t
do_mach_notify_port_destroyed (mach_port_t notify, mach_port_t name)
{
	return ill_rpc ("do_mach_notify_port_destroyed");
}

	error_t
do_mach_notify_send_once (mach_port_t notify)
{
	return ill_rpc ("do_mach_notify_send_once");
}
static struct target_ops gnu_target_ops = {
	gnu_create_inferior,
	gnu_attach,
	gnu_kill,
	gnu_detach,
	gnu_mourn,
	gnu_join,
	gnu_thread_alive,
	gnu_resume,
	gnu_wait,
	gnu_fetch_registers,
	gnu_store_registers,
	NULL,  /* prepare_to_access_memory */
	NULL,  /* done_accessing_memory */
	gnu_read_memory,
	gnu_write_memory,
	NULL,  /* look_up_symbols */
	gnu_request_interrupt,
	NULL,  /* read_auxv */
	NULL,  /* insert_point */
	NULL,  /* remove_point */
	NULL,  /* stopped_by_watchpoint */
	NULL,  /* stopped_data_address */
	NULL,  /* read_offsets */
	NULL,  /* get_tls_address */
	NULL,  /* qxfer_spu */
	NULL,  /* hostio_last_error */
	NULL,  /* qxfer_osdata */
	NULL,  /* qxfer_siginfo */
	NULL,  /* supports_non_stop */
	NULL,  /* async */
	NULL,  /* start_non_stop */
	NULL,  /* supports_multi_process */
	NULL,  /* handle_monitor_command */
};
void _initialize_gnu_nat (void)
{
	proc_server = getproc ();
}

static void initialize_low_arch()
{
	init_registers_i386 ();
	gnu_tdesc = tdesc_i386;
}

void initialize_low(void)
{
	set_target_ops (&gnu_target_ops);
	initialize_low_arch ();
	_initialize_gnu_nat();
}
