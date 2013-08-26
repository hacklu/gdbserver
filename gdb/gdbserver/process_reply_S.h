#ifndef	_process_reply_server_
#define	_process_reply_server_

/* Module process_reply */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>
#include <mach/mach_types.h>
#include <device/device_types.h>
#include <device/net_status.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <hurd/hurd_types.h>

/* SimpleRoutine proc_setmsgport_reply */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t S_proc_setmsgport_reply
(
	mach_port_t reply_port,
	kern_return_t return_code,
	mach_port_t oldmsgport
);

/* SimpleRoutine proc_getmsgport_reply */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t S_proc_getmsgport_reply
(
	mach_port_t reply_port,
	kern_return_t return_code,
	mach_port_t msgports
);

/* SimpleRoutine proc_wait_reply */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t S_proc_wait_reply
(
	mach_port_t reply_port,
	kern_return_t return_code,
	int status,
	int sigcode,
	rusage_t rusage,
	pid_t pid_status
);

#endif	/* not defined(_process_reply_server_) */
