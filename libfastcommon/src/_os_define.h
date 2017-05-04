#ifndef _OS_DEFINE_H
#define _OS_DEFINE_H

#define OS_BITS  64
#define OFF_BITS 64

#ifndef OS_FREEBSD
#define OS_FREEBSD  1
#endif

#ifndef IOEVENT_USE_KQUEUE
#define IOEVENT_USE_KQUEUE  1
#endif

#ifndef HAVE_VMMETER_H
#define HAVE_VMMETER_H 1
#endif

#ifndef HAVE_USER_H
#define HAVE_USER_H 1
#endif
#endif
