// stubs_lkl.c - Stubs for ksmbd-tools functions not needed in LKL mode
#include <tools.h>

int mountd_main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int addshare_main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int adduser_main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int control_main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	return 0;
}
