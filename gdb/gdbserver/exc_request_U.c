#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "exc_request_U.h"
#define EXPORT_BOOLEAN
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mach_types.h>
#include <mach/mig_errors.h>
#include <mach/mig_support.h>
#include <mach/msg_type.h>

#ifndef	mig_internal
#define	mig_internal	static
#endif

#ifndef	mig_external
#define mig_external
#endif

#ifndef	TypeCheck
#define	TypeCheck 1
#endif

#ifndef	UseExternRCSId
#define	UseExternRCSId		1
#endif

#define BAD_TYPECHECK(type, check) ({\
  union { mach_msg_type_t t; unsigned32_t w; } _t, _c;\
  _t.t = *(type); _c.t = *(check); _t.w != _c.w; })
#define msgh_request_port	msgh_remote_port
#define msgh_reply_port		msgh_local_port

#include <mach/std_types.h>

/* SimpleRoutine exception_raise_request */
mig_external kern_return_t exception_raise_request
(
	mach_port_t exception_port,
	mach_port_t reply,
	mach_msg_type_name_t replyPoly,
	mach_port_t thread,
	mach_port_t task,
	integer_t exception,
	integer_t code,
	integer_t subcode
)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t threadType;
		mach_port_t thread;
		mach_msg_type_t taskType;
		mach_port_t task;
		mach_msg_type_t exceptionType;
		integer_t exception;
		mach_msg_type_t codeType;
		integer_t code;
		mach_msg_type_t subcodeType;
		integer_t subcode;
	} Request;

	union {
		Request In;
	} Mess;

	register Request *InP = &Mess.In;


	auto const mach_msg_type_t threadType = {
		/* msgt_name = */		19,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	auto const mach_msg_type_t taskType = {
		/* msgt_name = */		19,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	auto const mach_msg_type_t exceptionType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	auto const mach_msg_type_t codeType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	auto const mach_msg_type_t subcodeType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	InP->threadType = threadType;

	InP->thread = thread;

	InP->taskType = taskType;

	InP->task = task;

	InP->exceptionType = exceptionType;

	InP->exception = exception;

	InP->codeType = codeType;

	InP->code = code;

	InP->subcodeType = subcodeType;

	InP->subcode = subcode;

	InP->Head.msgh_bits = MACH_MSGH_BITS_COMPLEX|
		MACH_MSGH_BITS(19, replyPoly);
	/* msgh_size passed as argument */
	InP->Head.msgh_request_port = exception_port;
	InP->Head.msgh_reply_port = reply;
	InP->Head.msgh_seqno = 0;
	InP->Head.msgh_id = 2400;

	return mach_msg(&InP->Head, MACH_SEND_MSG|MACH_MSG_OPTION_NONE, 64, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}
