#include "server.h"

#include <mach.h>
#include <mach_error.h>
#include <mach/exception.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/vm_attributes.h>

#include <hurd.h>
#include <hurd/interrupt.h>
#include <hurd/msg.h>
#include <hurd/msg_request.h>
#include <hurd/process.h>
/* All info needed to access an architecture/mode's registers.  */

struct regs_info
{
  /* Regset support bitmap: 1 for registers that are transferred as a part
     of a regset, 0 for ones that need to be handled individually.  This
     can be NULL if all registers are transferred with regsets or regsets
     are not supported.  */
  unsigned char *regset_bitmap;

  /* Info used when accessing registers with PTRACE_PEEKUSER /
     PTRACE_POKEUSER.  This can be NULL if all registers are
     transferred with regsets  .*/
  struct usrregs_info *usrregs;

#ifdef HAVE_gnu_REGSETS
  /* Info used when accessing registers with regsets.  */
  struct regsets_info *regsets_info;
#endif
};

#define ptid_of(proc) ((proc)->head.id)
#define pid_of(proc) ptid_get_pid ((proc)->head.id)
#define lwpid_of(proc) ptid_get_lwp ((proc)->head.id)

#define get_lwp(inf) ((struct lwp_info *)(inf))
#define get_thread_lwp(thr) (get_lwp (inferior_target_data (thr)))
#define get_lwp_thread(proc) ((struct thread_info *)			\
			      find_inferior_id (&all_threads,		\
						get_lwp (proc)->head.id))

#define PROC_TID_TASK 	(-1)
#define THREAD_STATE_FLAVOR		i386_REGS_SEGS_STATE
#define THREAD_STATE_SIZE		i386_THREAD_STATE_COUNT
#define THREAD_STATE_SET_TRACED(state) \
  	((struct i386_thread_state *) (state))->efl |= 0x100
#define THREAD_STATE_CLEAR_TRACED(state) \
  	((((struct i386_thread_state *) (state))->efl &= ~0x100), 1)

#define proc_is_task(proc) ((proc)->tid == PROC_TID_TASK)
#define proc_is_thread(proc) ((proc)->tid != PROC_TID_TASK)

#ifndef PIDGET
#define PIDGET(PTID) (ptid_get_pid (PTID))
#define TIDGET(PTID) (ptid_get_lwp (PTID))
#define MERGEPID(PID, TID) ptid_build (PID, TID, 0)
#endif

struct exc_state
  {
    int exception;		/* The exception code.  */
    int code, subcode;
    mach_port_t handler;	/* The real exception port to handle this.  */
    mach_port_t reply;		/* The reply port from the exception call.  */
  };

/* The results of the last wait an inf did.  */
struct inf_wait
  {
    struct target_waitstatus status;	/* The status returned to gdb.  */
    struct exc_state exc;	/* The exception that caused us to return.  */
    struct proc *thread;	/* The thread in question.  */
    int suppress;		/* Something trivial happened.  */
  };

struct proc
{
	thread_t port;		/* The task or thread port.  */
	int tid;			/* The GDB pid (actually a thread id).  */
	int num;			/* An id number for threads, to print.  */

	mach_port_t saved_exc_port;	/* The task/thread's real exception port.  */
	mach_port_t exc_port;	/* Our replacement, which for.  */

	int sc;			/* Desired suspend count.   */
	int cur_sc;			/* Implemented suspend count.  */
	int run_sc;			/* Default sc when the program is running.  */
	int pause_sc;		/* Default sc when gdb has control.  */
	int resume_sc;		/* Sc resulting from the last resume.  */
	int detach_sc;		/* SC to leave around when detaching
				   from program.  */

	thread_state_data_t state;	/* Registers, &c.  */
	int state_valid:1;		/* True if STATE is up to date.  */
	int state_changed:1;

	int aborted:1;		/* True if thread_abort has been called.  */
	int dead:1;			/* We happen to know it's actually dead.  */

	/* Bit mask of registers fetched by gdb.  This is used when we re-fetch
	   STATE after aborting the thread, to detect that gdb may have out-of-date
	   information.  */
	unsigned long fetched_regs;

	struct inf *inf;		/* Where we come from.  */

	struct proc *next;
};

struct inf
{
	/* Fields describing the current inferior.  */

	struct proc *task;		/* The mach task.   */
	struct proc *threads;	/* A linked list of all threads in TASK.  */

	/* True if THREADS needn't be validated by querying the task.  We
	   assume that we and the task in question are the only ones
	   frobbing the thread list, so as long as we don't let any code
	   run, we don't have to worry about THREADS changing.  */
	int threads_up_to_date;

	pid_t pid;			/* The real system PID.  */

	struct inf_wait wait;	/* What to return from target_wait.  */

	/* One thread proc in INF may be in `single-stepping mode'.  This
	   is it.  */
	struct proc *step_thread;

	/* The thread we think is the signal thread.  */
	struct proc *signal_thread;

	mach_port_t event_port;	/* Where we receive various msgs.  */

	/* True if we think at least one thread in the inferior could currently be
	   running.  */
	unsigned int running:1;

	/* True if the process has stopped (in the proc server sense).  Note that
	   since a proc server `stop' leaves the signal thread running, the inf can
	   be RUNNING && STOPPED...  */
	unsigned int stopped:1;

	/* True if the inferior has no message port.  */
	unsigned int nomsg:1;

	/* True if the inferior is traced.  */
	unsigned int traced:1;

	/* True if we shouldn't try waiting for the inferior, usually because we
	   can't for some reason.  */
	unsigned int no_wait:1;

	/* When starting a new inferior, we don't try to validate threads until all
	   the proper execs have been done.  This is a count of how many execs we
	   expect to happen.  */
	unsigned pending_execs;

	/* Fields describing global state.  */

	/* The task suspend count used when gdb has control.  This is normally 1 to
	   make things easier for us, but sometimes (like when attaching to vital
	   system servers) it may be desirable to let the task continue to run
	   (pausing individual threads as necessary).  */
	int pause_sc;

	/* The task suspend count left when detaching from a task.  */
	int detach_sc;

	/* The initial values used for the run_sc and pause_sc of newly discovered
	   threads -- see the definition of those fields in struct proc.  */
	int default_thread_run_sc;
	int default_thread_pause_sc;
	int default_thread_detach_sc;

	/* True if the process should be traced when started/attached.  Newly
	   started processes *must* be traced at first to exec them properly, but
	   if this is false, tracing is turned off as soon it has done so.  */
	int want_signals;

	/* True if exceptions from the inferior process should be trapped.  This
	   must be on to use breakpoints.  */
	int want_exceptions;
};

/* Forward decls */
struct inf *make_inf ();
void inf_clear_wait (struct inf *inf);
void inf_cleanup (struct inf *inf);
void inf_startup (struct inf *inf, int pid);
int inf_update_suspends (struct inf *inf);
void inf_set_pid (struct inf *inf, pid_t pid);
void inf_validate_procs (struct inf *inf);
void inf_steal_exc_ports (struct inf *inf);
void inf_restore_exc_ports (struct inf *inf);
struct proc *inf_tid_to_proc (struct inf *inf, int tid);
void inf_set_threads_resume_sc (struct inf *inf,
				struct proc *run_thread,
				int run_others);
int inf_set_threads_resume_sc_for_signal_thread (struct inf *inf);
void inf_suspend (struct inf *inf);
void inf_resume (struct inf *inf);
void inf_set_step_thread (struct inf *inf, struct proc *proc);
void inf_detach (struct inf *inf);
void inf_attach (struct inf *inf, int pid);
void inf_signal (struct inf *inf, enum gdb_signal sig);
void inf_continue (struct inf *inf);
int inf_update_procs (struct inf *inf);

void proc_abort (struct proc *proc, int force);
struct proc *make_proc (struct inf *inf, mach_port_t port, int tid);
struct proc *_proc_free (struct proc *proc);
int proc_update_sc (struct proc *proc);
error_t proc_get_exception_port (struct proc *proc, mach_port_t * port);
error_t proc_set_exception_port (struct proc *proc, mach_port_t port);
static mach_port_t _proc_get_exc_port (struct proc *proc);
void proc_steal_exc_port (struct proc *proc, mach_port_t exc_port);
void proc_restore_exc_port (struct proc *proc);
int proc_trace (struct proc *proc, int set);
static void inf_validate_task_sc (struct inf *inf);
static void inf_validate_procinfo (struct inf *inf);

//gdbserver use ptid_t not the same as gdb does!
static ptid_t gnu_ptid_build(int pid,long lwp,long tid);

//add for erase warning
extern const char * host_address_to_string (const void *addr);

extern int debug_flags;
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
