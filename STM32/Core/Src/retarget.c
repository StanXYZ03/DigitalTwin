#include <stdio.h>

/*
 * Keil/ARMCC semihosting uses BKPT 0xAB. If any C library function pulls in
 * semihosting, the firmware will stop in debug and can HardFault when running
 * standalone. These minimal retarget hooks make stdio harmless for this board.
 */
#if defined(__CC_ARM)
#pragma import(__use_no_semihosting)

struct __FILE
{
  int handle;
};

FILE __stdout;
FILE __stdin;

int fputc(int ch, FILE *f)
{
  (void)f;
  return ch;
}

int fgetc(FILE *f)
{
  (void)f;
  return 0;
}

int ferror(FILE *f)
{
  (void)f;
  return 0;
}

void _ttywrch(int ch)
{
  (void)ch;
}

void _sys_exit(int return_code)
{
  (void)return_code;
  while (1) {
  }
}
#endif

