#ifndef	_exc_server_
#define	_exc_server_

/* Module exc */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>

/* SimpleRoutine exception_raise_request */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t S_exception_raise_request
(
	mach_port_t exception_port,
	mach_port_t reply,
	mach_port_t thread,
	mach_port_t task,
	integer_t exception,
	integer_t code,
	integer_t subcode
);

#endif	/* not defined(_exc_server_) */
