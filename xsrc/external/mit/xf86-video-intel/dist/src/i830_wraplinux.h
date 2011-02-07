#ifndef _I830_WRAPLINUX_H_
#define _I830_WRAPLINUX_H_

#include <errno.h>

static __inline void
msleep(unsigned int msecs)
{
	if (usleep(1000*msecs) == EINVAL)
		FatalError("%s: invalid argument: %u\n", __FUNCTION__, msecs);
}

/*
 * Wait for COND to become true, timeout of MS milliseconds,
 * retry every 1 millisecond.
 */
#define wait_for(COND, MS) ({						\
	unsigned int i;							\
	int ret__ = 0;							\
	for (i = 0; ! (COND); i++) {					\
		if (i >= MS) {						\
			ret__ = ETIMEDOUT;				\
			break;						\
		}							\
		msleep(1);						\
	}								\
	ret__;								\
})

#define HAS_PCH_SPLIT(x)	IS_IGDNG(x)
#define HAS_PCH_CPT(x)		FALSE
#define IS_GEN5(x)		IS_IGDNG(x)
#define IS_GEN6(x)		FALSE

#define I915_WRITE(reg, val)	OUTREG(reg, val)
#define I915_READ(reg)		INREG(reg)
#define DRM_ERROR(fmt, arg...)		ErrorF(fmt, ## arg)
#define DRM_DEBUG_KMS(fmt, arg...)	DPRINTF(PFX, fmt, ## arg)

#endif /* _I830_WRAPLINUX_H_ */
