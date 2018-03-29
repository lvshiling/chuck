#include	<stdio.h>

extern const char	*const sys_errlist[];

#ifdef _MACH
extern const int sys_nerr;
#else
extern int sys_nerr;
#endif

char *strerror(int error)
{
	static char	mesg[30];

	if (error >= 0 && error <= sys_nerr)
		return((char*)sys_errlist[error]);

	snprintf(mesg, sizeof(mesg), "Unknown error (%d)", error);
	return(mesg);
}