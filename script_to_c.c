#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/*
### Added delay in stopping of command -Varun(28/05/2021)
## Changed $udp_in to array, Removed tmux capture-pane - Varun(25 Dec 2019)
## Seperate tmux for each language by listenting to broadcast packet
## Removed all the file checks to eliminate any delay - Varun (24-Dec-2019)
## Version .2 created on 11-jan-2018 by RY

## subtitle_automation receive script
#F1 - PLAY (Only Play for now - Packet will only be there if it's playing)
#F2 - Event_type - PRI, SEC, & LIVE (if any)
#F3 - Start_time - Event Start Time -, Wall Clock
#F4 - Clip_id - Event/Clip ID being played
#F5 - SubClip_id (if any)
#F6 - Title or NA if isn't available
#F7 - segment_no - Segment being played
#F8 - Reconcile_key - Unique Identifier if any
#F9 - playSom - Start SOM, if segmented then may differ from actual 
#Timecode of file/id/clip,
#F10 - playDuration - Duration to be played
#F11 - InPoint - Prepared to pass to MB-III as per Play SOM (F9)
#F12 - OutPoint - Prepared to pass to MB-III as per play SOM & Duration
#F13 - p/f - p if its a Program, f otherwise (i.e filler, commercial etc...)
# SUBTITLE_COMMAND_FORMAT
#ch_dvb_sub_originate.sh DESTNATIOn_IP:PORT LANG1 START_SECOND DURATION $1
*/
/*CH_ID=ch$1
char BASE[] = "/home/sub/DVBSUB";
cd $BASE
source $BASE/mux_header.bkup $1
langid=$2
session=${CH_ID}_v2_dvbsub
output_interface_ip="10.10.2.1"
src="bkup"*/

#define PORT	 9001 
#define MAXLINE 1024

unsigned int tokenize(const char* text, char delim, char*** output);
void reclaim2D(char ***store, unsigned int itemCount);


int hhmmss_to_secs(char *time) {
	int ctr = 0;
	int seconds=0;
	char * token = strtok(time, ":");
	printf( " %s\n", token ); //printing the token
	while( token != NULL ) {
		ctr++;
		if(ctr == 1)
		{
			seconds = seconds + atoi(token) * 3600;
		}
		else if(ctr == 2)
		{
			seconds = seconds + atoi(token) * 60;
		}
		else if(ctr == 3)
		{
			seconds = seconds + atoi(token);
		}
		else if(ctr == 4)
		{
			seconds = seconds + atoi(token) / 25;
		}
      
      token = strtok(NULL, ":");
	}
	printf("value of seconds:: %d\n",seconds);	
	return seconds;
}

void start_sub(long int SECONDS_S, char *ACT_DURATION, char *SUB_ID, char *languages, char *CH_ID, char * COMMAND) 
{
	char DURATION[50];
	char ID=SUB_ID;
	char *lang_code=languages;
	char *ch_id=CH_ID;
	char *command = COMMAND;
	int sec = SECONDS_S;
	long int tm,END_secs,OFFSET=1;
	//float ADVANCED = 819.66000000000000000000;
	double ADVANCED = 0;
	long int START;
	long int END;
	float DELAYSET=2.5;
	char buff[100];
    	time(&tm);
	printf("value of ACT_DURATION: %s\n",ACT_DURATION);
	strcpy(DURATION,ACT_DURATION);
	//printf("Cmd Time: %s\nID\t:\t%s\nSTART\t:\t%s\nDUR.\t:\t%s\n", ctime(&tm), ID, START, DURATION);
	if(sec != 0 )
	{ 
		printf("SECONDS_S: %d\n",SECONDS_S);
		ADVANCED = ADVANCED + SECONDS_S;
		START = 27000000 * ADVANCED + OFFSET;
		//gcvt(DURATION, 6,buff);
		//char buff1[] = "0";
		//strcpy(buff,buff1);
		printf("value of duration: %s\n",DURATION);
		//END_secs = hhmmss_to_secs(buff);
		END_secs = hhmmss_to_secs(DURATION);
		printf("test==>\n");
		END = 27000000 * (END_secs - DELAYSET);	//Value of fixed delay subtracted from duration
		char S_COMMAND[500]; 
		sprintf(S_COMMAND,"multicat_ch1_sim -U -p 1001 -k %ld -d %ld /sub_data/rewind/TS/RWD-PG-003770_sim.ts -U 226.1.1.1:2001@10.10.1.1",START,END);
		char lang[] = "sim";
		//printf("data in languages: %s\n",languages);
		//strcpy(lang,languages);
	//	printf("test-13\n");
		printf("ADVANCED: %f\n",ADVANCED);
		printf("START: %ld\n",START);
		printf("END_secs: %d\n",END_secs);
		printf("END: %ld\n",END);
		printf("S_COMMAND: %s\n",S_COMMAND);
		printf("lang: %s\n",lang);
	}
	/*
		S_COMMAND="multicat_${ch_id}_${lang} -U -p 1001 -k ${START} -d ${END} $SPATH/${ID}_${lang}.ts -U ${SUB_IP}:`expr ${SUB_BASE} + ${langid}`@${output_interface_ip}"
		run_status=`ps -ef | grep multicat_${ch_id}_${lang} | grep -v grep`
		e_status=$?
		if (e_status == 0)
		{
			id_running=`echo $run_status | cut -d" " -f15-15`
			if (id_running == "${SPATH}/${ID}_${lang}.ts")
			{
				printf("\nSTATUS: Running\n");
			}	
			else
			{
				STOP=true;
			}
		}
		else	
		{
			tmux send-keys -t ${session}:0.`expr $(( sub_count - 1 + langid ))` "clear" C-m
			if (!strcmp(command, "START"))
			{
				DELAY = START_DELAYSET;
				
				sleep(START_DELAYSET);
			}
			else
			{
				DELAY = DELAYSET;
				sleep(DELAYSET);
			}
			tmux send-keys -t ${session}:0.`expr $(( sub_count - 1 + langid ))` "${S_COMMAND}" C-m
		}
	}
	else
	{
		printf("ch_id : %s, Stopping\n");
		tmux send-keys -t ${session}:0.`expr $(( sub_count - 1 + langid ))` C-c
		tmux send-keys -t ${session}:0.`expr $(( sub_count - 1 + langid ))` C-c
	}*/

	printf("code completed\n");
}

unsigned int tokenize(const char* text, char delim, char*** output) {
	if((*output) != NULL) return -1; /* I will allocate my own storage */
	int ndelims,i,j,ntokens,starttok,endtok;

	// First pass, count the number of delims
	i=0;
	ndelims=0;
	while(text[i] != '\0') {
		if(text[i] == delim) ndelims++;
		i++;
	}

	// The number of delims is one less than the number of tokens
	ntokens=ndelims+1;

	// Now, allocate an array of (char*)'s equal to the number of tokens

	(*output) = (char**) malloc(sizeof(char*)*ntokens);

	// Now, loop through and extract each token
	starttok=0;
	endtok=0;
	i=0;
	j=0;
	while(text[i] != '\0') {
		// Reached the end of a token?
		if(text[i] == delim) {
			endtok = i;
			// Allocate a char array to hold the token
			(*output)[j] = (char*) malloc(sizeof(char)*(endtok-starttok+1));
			// If the token is not empty, copy over the token
			if(endtok-starttok > 0)
				memcpy((*output)[j],&text[starttok],(endtok-starttok));
			// Null-terminate the string
			(*output)[j][(endtok-starttok)] = '\0';
			// The next token starts at i+1
			starttok = i+1;
			j++;
		}
		i++;
	}

	// Deal with the last token
	endtok = i;
	// Allocate a char array to hold the token
	(*output)[j] = (char*) malloc(sizeof(char)*(endtok-starttok+1));
	// If the token is not empty, copy over the token
	if(endtok-starttok > 0)
		memcpy((*output)[j],&text[starttok],(endtok-starttok));
	// Null-terminate the string
	(*output)[j][(endtok-starttok)] = '\0';
	
	return ntokens;

}

void reclaim2D(char ***store, unsigned int itemCount)
{
	int x;
    for (x = 0; itemCount < itemCount; ++x)
    {
        if((*store)[x] != NULL) free((*store)[x]);
        (*store)[x] = NULL;
    }

    if((*store) != NULL) free((*store));
    (*store) = NULL;
}

void main()
{
	int sockfd; 
	int len, n,c,i=0;
	char buffer[MAXLINE]; 
	char udp_in[MAXLINE];
	char *myarr[MAXLINE]={0};
	long int FRAMES_S;
	char SUB_ID[100];
	char segment_number[100];
	char segment_time[100];
	char DURATION_REMAIN[100];
	//float ACT_DURATION;
	char ACT_DURATION[100];
	char COMMAND[100];
	char P_TYPE[100];
	char **tokens = NULL;
	int DELAYSET=2.5;
	long int segment_time_in_secs,SOM_in_secs=36000;
	long int start_pos,CH_ID,SECONDS_S; 	
	char languages;

	struct sockaddr_in servaddr, cliaddr; 

	
	if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
		perror("socket creation failed"); 
		exit(EXIT_FAILURE); 
	} 
	
	memset(&servaddr, 0, sizeof(servaddr)); 
	memset(&cliaddr, 0, sizeof(cliaddr)); 
		 
	servaddr.sin_family = AF_INET; 
	servaddr.sin_addr.s_addr = INADDR_ANY; 
	servaddr.sin_port = htons(PORT); 
	
	if ( bind(sockfd, (const struct sockaddr *)&servaddr, 
			sizeof(servaddr)) < 0 ) 
	{ 
		perror("bind failed"); 
		exit(EXIT_FAILURE); 
	} 
	len = sizeof(cliaddr);
	while(1)
	{
		printf("Waiting for socket data:\n");
		
		//my_nc -l -w0 -u ${automation_port} | while read udp_in 
		n = recvfrom(sockfd, (char *)buffer, MAXLINE, 
					MSG_WAITALL, ( struct sockaddr *) &cliaddr, 
					&len); 
		buffer[n] = '\0'; 
		printf("%d Client : %s\n",i++, buffer);
		memset(udp_in, '\0', sizeof(udp_in));
		strcpy(udp_in, buffer);
		char* strSplit = udp_in;
		while(1)
		{
			printf("udp_in : %s\n", udp_in);
			
			//printf("string : '%s'\n",udp_in);
			c=tokenize(udp_in,',',&tokens);
			for(i=0;i<c;i++) {
				//printf("token %d: '%s'\n",i+1,tokens[i]);
				myarr[i] = tokens[i];

			}
			reclaim2D(&tokens, c);			

			//for(i=0; i<c; i++)
       			//	printf("i ==%d %s\n",i,myarr[i]);
			
			printf("data on myarr[0]: %s\n",myarr[0]);
			strcpy(COMMAND, myarr[0]);
			strcpy(P_TYPE,myarr[12]);
			printf("\nP_TYPE: %s\n",P_TYPE);
			printf("length od p_type: %d\n",strlen(P_TYPE));
			if(!strcmp(P_TYPE,"p"))
			{
				FRAMES_S = atoi(myarr[10]);
				printf("FRAMES_S: %d\n",FRAMES_S);
				SECONDS_S = (FRAMES_S / 25);
				printf("SECONDS_S: %d\n",SECONDS_S);
				strcpy(SUB_ID,myarr[3]);
				printf("SUB_ID: %s\n",SUB_ID);
				strcpy(segment_number,myarr[6]);
				strcpy(segment_time,myarr[8]);	//#segment start time in hh:mm:ss:ff
				printf("segment_time: %s\n",segment_time);
				segment_time_in_secs = hhmmss_to_secs(segment_time);	//Segment start time in seconds
				start_pos = segment_time_in_secs - SOM_in_secs;
				printf("start_pos: %d\n",start_pos);				
				strcpy(DURATION_REMAIN,myarr[9]);
				printf("DURATION_REMAIN %s\n",DURATION_REMAIN);
				strncpy(ACT_DURATION,DURATION_REMAIN,8);
				printf("ACT_DURATION: %s\n",ACT_DURATION);				
				//DELAYSET = DELAYSET;
				SECONDS_S = SECONDS_S + start_pos;
				if (!strcmp(COMMAND,"START"))
					start_sub(0,0,0,0,0,0);

				start_sub(SECONDS_S, ACT_DURATION, SUB_ID, languages, CH_ID, COMMAND);
			}
			else if ( !strcmp(P_TYPE,"f") || ( !strcmp(COMMAND,"END") || !strcmp(COMMAND,"STOP") || !strcmp(COMMAND,"SKIP")))
			{
				sleep(DELAYSET-1);
				start_sub(0,0,0,0,0,0);
			}
		}
	}
}

