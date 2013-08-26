#ifndef	_notify_server_
#define	_notify_server_

/* Module notify */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>

/* SimpleRoutine mach_notify_port_deleted */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_port_deleted
(
	mach_port_t notify,
	mach_port_t name
);

/* SimpleRoutine mach_notify_msg_accepted */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_msg_accepted
(
	mach_port_t notify,
	mach_port_t name
);

/* SimpleRoutine mach_notify_port_destroyed */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_port_destroyed
(
	mach_port_t notify,
	mach_port_t rights
);

/* SimpleRoutine mach_notify_no_senders */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_no_senders
(
	mach_port_t notify,
	mach_port_mscount_t mscount
);

/* SimpleRoutine mach_notify_send_once */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_send_once
(
	mach_port_t notify
);

/* SimpleRoutine mach_notify_dead_name */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t do_mach_notify_dead_name
(
	mach_port_t notify,
	mach_port_t name
);

#endif	/* not defined(_notify_server_) */
