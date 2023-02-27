#include <stdio.h>
#include <time.h>
//#include "debug_log.h"

//unsigned char log_run_level = LOG_LVL_DEBUG;
/*
const char * log_level_strings [] = {
    "NONE", // 0
    "CRIT", // 1
    "WARN", // 2
    "NOTI", // 3
    " LOG", // 4
    "DEBG" // 5
};*/

char *timestamp()
{
	int i = 0;
	time_t ltime;
	struct tm result;
	char stime[32];
	char raw_time[100];
	char *final_time = malloc(25*sizeof(char));
	memset(raw_time, '\0', 100);
//	memset(final_time, '\0', 100);

	ltime = time(NULL);
	localtime_r(&ltime, &result);
	strcpy(raw_time, asctime_r(&result, stime));
	while(1)
	{
			if(raw_time[i] == '\n')
					break;
			final_time[i] = raw_time[i];
			i++;
	}

	return final_time;
}

/*
void main()
{
		log_set = 1;
		printf("Calling debug functions****\n");
		LOG(1,"This print id coming from main with value : %d",1001);
		LOG(2,"This print id coming from main with value : %d",1002);
}*/
