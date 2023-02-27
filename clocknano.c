#include <time.h>
#include <stdio.h>

int main()
{
  long long start,endt;
  struct timespec time, end;
  clock_gettime(CLOCK_MONOTONIC, &time);
  printf("%lld.%.9ld seconds have elapsed!", (long long) time.tv_sec, time.tv_nsec);
  start=(long long) time.tv_sec*1000000000+time.tv_nsec;
  //printf("\nOR \n%d seconds and %ld nanoseconds have elapsed!", time.tv_sec, time.tv_nsec);
  sleep(5);
  clock_gettime(CLOCK_MONOTONIC, &end);
  printf("\n%lld.%.9ld seconds have elapsed!", (long long) end.tv_sec, end.tv_nsec);
  //printf("\nOR \n%d seconds and %ld nanoseconds have elapsed!", end.tv_sec, end.tv_nsec);
  endt=(long long) end.tv_sec*1000000000+end.tv_nsec;
  printf("\n%.lld",endt-start);
  return 0;
}
