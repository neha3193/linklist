/*****************************************************************************
 * multicat.c: netcat-equivalent for multicast
 *****************************************************************************
 * Copyright (C) 2009, 2011-2012, 2015-2017 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/***********************************TODO***************************************
1.	Code to add trigger based choice to receive from 2(A/B, say A is main and B is backup) 
    inputs of Automation command(START, STOP...).

2.	Add auto mode for point no.1. In auto mode threshold value will decide to switch from 
    input A to B(or vice a versa).

3.	Code to add trigger based choice to receive 2 live streams for reference.

4.	(Informative point)All triggers will work independently.

5.	Config file will have debug ON/OFF functionality for multicat code debug prints.

6.	(Informative point) In real life scenario also, application will "ts" files for 
    subtitle input.

7.	Zabbix integration, we'll deal it later.

8.	Ncurses, a command line based UI needs to be developed using ncurses which will have 
    multiple information like current inputs, name of running channel etc.

9.	(Informative point)Sub sender block(in architecture diagram) is multicat module.

10.	Code to add trigger based output(main or backup).

11.	Information about configuration of Main and Backups to be read from conf file.
******************************************************************************/
/* POLLRDHUP */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <curl/curl.h>

#ifndef SYS_gettid
#error "SYS_gettid unavailable on this system"
#endif

#define gettid() ((pid_t)syscall(SYS_gettid))

#define PORT 9001
#define PORT_BACKUP 9002

#define Backup_Port 9004

#define TS_PACKET_SIZE 188

#define MAXLINE 1024
#define MAXDATA 100
#define COMMAND_LENGTH 200
#define UDP_MAXIMUM_SIZE 1316
#define MAX_PID_LEN 32
#ifdef SIOCGSTAMPNS
#define HAVE_TIMESTAMPS
#endif

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

#include "../bitstream/ietf/rtp.h"
#include "../bitstream/mpeg/ts.h"
#include "../bitstream/mpeg/pes.h"
#include "util.h"

int debug_var = 0;
#ifdef DEBUG
#define DEBUG_PRINTF(...) if(debug_var == 1){fprintf(stderr,"DEBUG: "__VA_ARGS__);}
#else
#define DEBUG_PRINTF(...)
#endif

#undef DEBUG_WRITEBACK
#define POLL_TIMEOUT 1000              /* 1 s */
#define MAX_LATENESS INT64_C(27000000) /* 1 s */
#define FILE_FLUSH INT64_C(2700000)    /* 100 ms */
#define MAX_PIDS 8192
#define POW2_33 UINT64_C(8589934592)
#define TS_CLOCK_MAX (POW2_33 * 27000000 / 90000)
#define MAX_PCR_INTERVAL (27000000 / 2)

#define MAX_LANG 10
#define S_COMMAND_LENGTH 123
#define IP_LENGTH 16
#define PORT_LENGTH 5
#define PID_LENGTH 5
#define MAX_WORD 50
#define PACKET_SIZE 188

int create_thread[10];
static int thread_is_live = 1;
void *multicat(void *argv);
/*****************************************************************************
 * Local declarations
 *****************************************************************************/
FILE *p_output_aux;
static int i_ttl = 0;
static bool b_sleep = true;
static uint16_t i_pcr_pid = 0;
static bool b_overwrite_ssrc = false;
static in_addr_t i_ssrc = 0;
static bool b_input_udp = false, b_output_udp = false;
static size_t i_asked_payload_size = DEFAULT_PAYLOAD_SIZE;
static size_t i_rtp_header_size = RTP_HEADER_SIZE;
static uint64_t i_rotate_size = DEFAULT_ROTATE_SIZE;
static uint64_t i_rotate_offset = DEFAULT_ROTATE_OFFSET;
static uint64_t i_duration = 0;
static struct udprawpkt pktheader;
static bool b_raw_packets = false;
static uint8_t *pi_pid_cc_table = NULL;

/* PCR/PTS/DTS restamping */
static uint64_t i_last_pcr_date;
static uint64_t i_last_pcr = TS_CLOCK_MAX;
static uint64_t i_pcr_offset;

static volatile sig_atomic_t b_die = 0, b_error = 0;
static uint16_t i_rtp_seqnum;
static uint64_t i_stc = 0; /* system time clock, used for date calculations */
static uint64_t i_first_stc = 0;
static uint64_t i_pcr = 0, i_pcr_stc = 0; /* for RTP/TS output */
static uint64_t (*pf_Date)(void) = wall_Date;
static void (*pf_Sleep)(uint64_t) = wall_Sleep;
static ssize_t (*pf_Read)(void *p_buf, size_t i_len, FILE *p_input_aux, int i_input_fd);
static bool (*pf_Delay)(void) = NULL;
static void (*pf_ExitRead)(int i_input_fd);
static ssize_t (*pf_Write)(const void *p_buf, size_t i_len, int i_output_fd);
static void (*pf_ExitWrite)(int i_output_fd);
void start_sub(long int SECONDS_S, char *ACT_DURATION, char *SUB_ID, long int *value);

int toggle_val = 0;
int gTotalLanguage;
int gUseBackupScommand;
int gAutomationDataReceived;
char gAutomationbuffer[PACKET_SIZE];
char debug_print[10];

struct scriptArguments {
    char Language[MAX_LANG];
    char Subtitle[MAXDATA];
    char ReferenceIp[IP_LENGTH];
    char BackupReferenceIp[IP_LENGTH];
    char AutomationToggleCommandIP[IP_LENGTH];
	char UseAutomationBackup[4];
    int ReferencePort;
    int BackupReferencePort;
    int ReferencePID;
    int ReplacePID;
    int SubtitlePID;
    int Offset;
    int AutomationToggleCommandPort;
    char MainIPAutomation[IP_LENGTH];
    int MainPortAutomation;
    char BackupIPAutomation[IP_LENGTH];
    int BackupPortAutomation;
    char IPOutputToMux[IP_LENGTH];
    int PortOutputToMux;
};
struct scriptArguments inputArgsAllLanguages[MAX_LANG];
struct scriptArguments *inputArgs;

/**************************************************************************************
                                Zabbix implimentation
 **************************************************************************************/

char gAuth_code[100];
char gItem_code[50];
char gZABBIX_IP;
char gZABBIX_USERNAME[20];
char gZABBIX_PASSWORD[20];
char gCHANNEL_NAME[64];
char gHOST_NAME[64];
char gCurl_url[200];
int debug = 0;

struct string {
	char *ptr;
	size_t len;
};
void init_string(struct string *s) {
	s->len = 0;
	s->ptr = malloc(s->len+1);
	if (s->ptr == NULL) {
		fprintf(stderr, "malloc() failed\n");
		exit(EXIT_FAILURE);
	}
	s->ptr[0] = '\0';
}

size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s)
{
	size_t new_len = s->len + size*nmemb;
	s->ptr = realloc(s->ptr, new_len+1);
	if (s->ptr == NULL) {
		fprintf(stderr, "realloc() failed\n");
		exit(EXIT_FAILURE);
	}
	memcpy(s->ptr+s->len, ptr, size*nmemb);
	s->ptr[new_len] = '\0';
	s->len = new_len;

	return size*nmemb;
}

void update_item(int update_item_flag)
{

	char cmd[500];
	sprintf(cmd,"zabbix_sender -vv -z 192.168.25.163 -s \"%s\" -k injector.%s -o 0",gHOST_NAME,gCHANNEL_NAME);
        printf("\nUpdating item...\n");
	system(cmd);
        printf("\nUpdated successfully\n");
}
void host_id(void)
{
	CURL *curl;
	CURLcode res;

	int token_count;
	struct curl_slist *slist1;
	/* get a curl handle */

	curl = curl_easy_init();
	if(curl) {
		/* First set the URL that is about to receive our POST. This URL can
		   just as well be a https:// URL if that is what should receive the
		   data. */
		struct string s;
		init_string(&s);

		slist1 = NULL;
		slist1 = curl_slist_append(slist1, "Content-Type: application/json");
        sprintf(gCurl_url,"http://%s/zabbix/api_jsonrpc.php",gZABBIX_IP);
		curl_easy_setopt(curl, CURLOPT_URL, gCurl_url);
		/* Now specify the POST data */

		char buf[200];

		sprintf(buf, "{\"jsonrpc\": \"2.0\", \"method\": \"host.get\", \"params\": {\"output\": [\"hostid\"],\"filter\": {\"host\": [ \"scte_injector_zabbix_test\" ]}} ,\"id\": 1, \"auth\": \"%s\"}", gAuth_code);
		//printf("{\"jsonrpc\": \"2.0\", \"method\": \"host.get\", \"params\": {\"output\": [\"hostid\"],\"filter\": {\"host\": [ \"scte_injector_zabbix_test\" ]}} ,\"id\": 1, \"auth\": \"abcd\"}");
		
		if(debug == 1)
			printf("JSON DATA: [%s]\n",buf);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist1);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, "1L");

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* Check for errors */

		if(res != CURLE_OK)
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
					curl_easy_strerror(res));
		if(debug == 1){
			printf("RETURN DATA : [%s]\n", s.ptr);
			printf("RETURN LEN : [%zd]\n", s.len);
		}
		free(s.ptr);
		/* always cleanup */
		curl_easy_cleanup(curl);
	}
	return 0;
}


void host_item(void)
{
	CURL *curl;
	CURLcode res;

	char *token;
	int token_count;
	char *temp_str;
	token_count = 0;

	struct curl_slist *slist1;
	/* get a curl handle */

	curl = curl_easy_init();
	if(curl) {
		/* First set the URL that is about to receive our POST. This URL can
		   just as well be a https:// URL if that is what should receive the
		   data. */
		struct string s;
		init_string(&s);

		slist1 = NULL;
		slist1 = curl_slist_append(slist1, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.25.163/zabbix/api_jsonrpc.php");
		/* Now specify the POST data */

		char buf[200];

		sprintf(buf, "{\"jsonrpc\": \"2.0\", \"method\": \"item.get\", \"params\": {\"hostids\":\"12537\",\"search\": {\"key_\": \"injector\"}} ,\"id\": 1, \"auth\": \"%s\"}", gAuth_code);
		
		if(debug == 1)
			printf("JSON DATA: [%s]\n",buf);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist1);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, "1L");

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* Check for errors */

		if(res != CURLE_OK)
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
					curl_easy_strerror(res));

		temp_str = malloc(s.len+1);
		memcpy(temp_str, s.ptr, s.len);
		temp_str[s.len] = '\0';
		
		if(debug == 1)
			printf("\nTemp : %s\n", temp_str);

		token = strtok(temp_str, "\"");
		while(token != NULL)
		{
			token = strtok(NULL, "\"");
			token_count++;
			if(token_count == 9)
				strcpy(gItem_code, token);
			//                        printf("\n%s, count : %d",token,token_count);
		}


		if(debug == 1){
			printf("RETURN DATA : [%s]\n", s.ptr);
			printf("RETURN LEN : [%zd]\n", s.len);
		}
		free(s.ptr);
		/* always cleanup */
		curl_easy_cleanup(curl);
	}
	return 0;
}

int curl(void)
{
	CURL *curl;
	CURLcode res;

	char *token;
	int token_count;
	struct curl_slist *slist1;
	/* get a curl handle */

	char *temp_str;
	token_count = 0;
	curl = curl_easy_init();
	if(curl) {
		/* First set the URL that is about to receive our POST. This URL can
		   just as well be a https:// URL if that is what should receive the
		   data. */
		struct string s;
		init_string(&s);

		slist1 = NULL;
		slist1 = curl_slist_append(slist1, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.25.163/zabbix/api_jsonrpc.php");
		/* Now specify the POST data */

		
		char buf[400];
		sprintf(buf,"{\"jsonrpc\": \"2.0\", \"method\": \"user.login\", \"params\": {\"user\":\"%s\", \"password\":\"%s\"}, \"id\": 1, \"auth\": null}",gZABBIX_USERNAME,gZABBIX_PASSWORD);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist1);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, "1L");

		if(debug == 1)
			printf("JSON DATA: [%s]\n",buf);
	
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* Check for errors */

		if(res != CURLE_OK)
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
					curl_easy_strerror(res));
		temp_str = malloc(s.len+1);
		memcpy(temp_str, s.ptr, s.len);
		temp_str[s.len] = '\0';
		
		if(debug == 1)
			printf("\nTemp : %s\n", temp_str);

		token = strtok(temp_str, "\"");
		while(token != NULL)
		{
			token = strtok(NULL, "\"");
			token_count++;
			if(token_count == 7)
				strcpy(gAuth_code, token);
			//      printf("\n%s, count : %d",token,token_count);
		}
		if(debug == 1){
			printf("RETURN DATA : [%s]\n", s.ptr);
			printf("RETURN LEN : [%zd]\n", s.len);
			printf("\nAuth : [%s]\n", gAuth_code);
		}
		free(s.ptr);
		/* always cleanup */
		curl_easy_cleanup(curl);
	}
	host_id();
	host_item();
	return 0;
}
/*************************************************************************
                Zabbix code ends here
**************************************************************************/

/**************************************************************************
*		ptsreplacement code
**************************************************************************/
int size = 2;
unsigned long long int nPTS[MAX_LANG];

struct data
{
    int oprFlag;
    char IP[IP_LENGTH];
    char PORT_P[PORT_LENGTH];
    int *PID;
    int lang_id;
    unsigned char packet[PACKET_SIZE];
    double offSet;
};
int inPidList(int myPid, int PID)
{
    int i;

    for (i = 0; i < size; i++)
    {
        if (!PID)
        {
            return (0);
        }
        if (myPid == PID)
            return (1);
    }
    return (0);
}

#if DEBUG
    static const char *secToTime(long int tSeconds)
    {
        long int tMinutes = 0;
        long int tMinutesT = 0;
        long int tHours = 0;
        static char tDuration[12];
        tHours = tSeconds / 3600;
        tMinutesT = tSeconds % 3600;
        tMinutes = tMinutesT / 60;
        tSeconds = tMinutesT % 60;
        sprintf(tDuration, "%02ld:%02ld:%02ld", tHours, tMinutes, tSeconds);
        return (tDuration);
    }
#endif

/*changes:startIndicator-->argument indicates start of PES header*/
int changePts(unsigned long long int nPTS, unsigned char *packet, int startIndicator)
{
    char lPTS = 0;
    if ((packet[3] & 0x30) == 0x10)
    {
        lPTS = ((nPTS >> 30) & 0x0000000000000007);
        packet[13] = (lPTS << 1) | (packet[13] & 0xf1);

        lPTS = ((nPTS >> 22) & 0x00000000000000ff);
        packet[14] = lPTS;

        lPTS = ((nPTS >> 15) & 0x00000000000000ff);
        packet[15] = (lPTS << 1) | (packet[15] & 0x01);

        lPTS = ((nPTS >> 7) & 0x00000000000000ff);
        packet[16] = lPTS;

        lPTS = (nPTS & 0x00000000000000ff);
        packet[17] = (lPTS << 1) | (packet[17] & 0x01);
    }
    else if ((packet[3] & 0x30) == 0x30)
    {
        lPTS = ((nPTS >> 30) & 0x0000000000000007);
        packet[startIndicator + 6] = (lPTS << 1) | (packet[startIndicator + 6] & 0xf1);

        lPTS = ((nPTS >> 22) & 0x00000000000000ff);
        packet[startIndicator + 7] = lPTS;

        lPTS = ((nPTS >> 15) & 0x00000000000000ff);
        packet[startIndicator + 8] = (lPTS << 1) | (packet[startIndicator + 8] & 0x01);

        lPTS = ((nPTS >> 7) & 0x00000000000000ff);
        packet[startIndicator + 9] = lPTS;

        lPTS = (nPTS & 0x00000000000000ff);
        packet[startIndicator + 10] = (lPTS << 1) | (packet[startIndicator + 10] & 0x01);
    }
    return 1;
}

clock_t tsRefAutoSwitchTimeStarts;// = clock();
unsigned char gUDP_Packet[UDP_MAXIMUM_SIZE];
int gLength;
void *tsRefAutoSwitch(void *arg)
{
	int msec = 0, trigger = 30*1000; /* 10ms */
	int iterations = 0;
	while(1)
	{
		tsRefAutoSwitchTimeStarts = clock();
#if 0	
		do {
				clock_t difference = clock() - tsRefAutoSwitchTimeStarts;
				msec = difference * 1000 / CLOCKS_PER_SEC;
				iterations++;
		} while ( msec < trigger );

		if(toggle_val == 0)
		{
			toggle_val = 1;
			fprintf(stderr, "Listening from backup tsReference input\n");
		}
		else
		{
			toggle_val = 0;
                    if(sent <= 0) CKET_SIZE
			fprintf(stderr, "Listening from main tsReference input\n");
		}
		
		printf("Time taken %d seconds %d milliseconds (%d iterations)\n",
		msec/1000, msec%1000, iterations);
		msec = 0;
#endif
	}
//	exit(0);
}

void *tsRefMainInput(void *Data)
{
	unsigned char UDP_Packet[UDP_MAXIMUM_SIZE];
    int LanguageId = 0;
	//LanguageId = *((int *)Data);

	int sockfd = 0;
    struct sockaddr_in addr;
#ifdef HAVE_IP_MREQN
    struct ip_mreqn mgroup;
#else
    struct ip_mreq mgroup;
#endif
    int reuse;
    unsigned int addrlen;
//    int len;
    //unsigned char udp_packet[UDP_MAXIMUM_SIZE];

    memset((char *)&mgroup, 0, sizeof(mgroup));
    mgroup.imr_multiaddr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].ReferenceIp);
#ifdef HAVE_IP_MREQN
    mgroup.imr_address.s_addr = INADDR_ANY;
#else
    mgroup.imr_interface.s_addr = INADDR_ANY;
#endif
    memset((char *)&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(inputArgsAllLanguages[LanguageId].ReferencePort);
    addr.sin_addr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].ReferenceIp);
    addrlen = sizeof(addr);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	fprintf(stderr, "Main reference IP : [%s]\n",inputArgsAllLanguages[LanguageId].ReferenceIp);
	fprintf(stderr, "Main reference PORT : [%d]\n",inputArgsAllLanguages[LanguageId].ReferencePort);
    if (sockfd < 0)
    {
        perror("socket(): error ");
        return 0;
    }
    reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt() SO_REUSEADDR: error ");
    }

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind(): error");
        close(sockfd);
        return 0;
    }
/*
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mgroup, sizeof(mgroup)) < 0)
    {
        perror("setsockopt() IPPROTO_IP: error ");
        close(sockfd);
        return 0;
    }
*/
	printf("TEST MAIN, toggle_val : %d\n", toggle_val);
	while(1)
	{	
	fprintf(stderr, "Before receiving Main ref data, Sockfd : %d\n", sockfd);
	    memset(gUDP_Packet, '\0', sizeof(gUDP_Packet));
		gLength = recvfrom(sockfd, gUDP_Packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *)&addr, &addrlen);
	fprintf(stderr, "After receiving Main ref data \n");
			//fprintf(stderr,"tsRefAutoSwitchTimeStarts before reset\n");
			//tsRefAutoSwitchTimeStarts = clock();
//			fprintf(stderr,"m1\n");
            //gLength = recvfrom(sockfd, gUDP_Packet, UDP_MAXIMUM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addrlen);
			//tsRefAutoSwitchTimeStarts = clock();
			/*printf("value of len: %d\t",len);*/
            toggle_val = 1;
		if (toggle_val == 0)
		{
			memset(gUDP_Packet, '\0', sizeof(gUDP_Packet));
			strcpy(gUDP_Packet, UDP_Packet);
			//fprintf(stderr,"m2 gLength : %d\t", gLength);
			//sleep(1);
		}
	}	
}

void *tsRefBackupInput(void *Data)
{
	unsigned char UDP_Packet[UDP_MAXIMUM_SIZE];
	int LanguageId = 0;

	if(LanguageId > 0)
	{
    	LanguageId = LanguageId - 1;
	}

	int sockfd1 = 0;
    struct sockaddr_in addr;
#ifdef HAVE_IP_MREQN
    struct ip_mreqn mgroup;
#else
    struct ip_mreq mgroup;
#endif
    int reuse, reuse1;
    unsigned int addrlen;
//    int len;
    int idx;

    memset((char *)&mgroup, 0, sizeof(mgroup));
    mgroup.imr_multiaddr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].BackupReferenceIp);
#ifdef HAVE_IP_MREQN
    mgroup.imr_address.s_addr = INADDR_ANY;
#else
    mgroup.imr_interface.s_addr = INADDR_ANY;
#endif
	
	/*Code for backup socket*/
	memset((char *)&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(inputArgsAllLanguages[LanguageId].BackupReferencePort);
	addr.sin_addr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].BackupReferenceIp);
	addrlen = sizeof(addr);
	sockfd1 = socket(AF_INET, SOCK_DGRAM, 0);
	fprintf(stderr, "Backup reference IP : [%s]\n",inputArgsAllLanguages[LanguageId].BackupReferenceIp);
	fprintf(stderr, "Backup reference PORT : [%d]\n",inputArgsAllLanguages[LanguageId].BackupReferencePort);
	printf("TEST 1\n");
	if (sockfd1 < 0)
	{
			perror("socket(): error ");
			return 0;
	}
	reuse1 = 1;
	printf("TEST 2\n");
	if (setsockopt(sockfd1, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse1, sizeof(reuse1)) < 0)
	{
			perror("setsockopt() SO_REUSEADDR: error ");
	}
	printf("TEST 3\n");
	if (bind(sockfd1, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
			perror("bind(): error");
			close(sockfd1);
			return 0;
	}
/*
	if (setsockopt(sockfd1, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mgroup, sizeof(mgroup)) < 0)
	{
			perror("setsockopt() IPPROTO_IP: error ");
			close(sockfd1);
			return 0;
	}
*/
	/*Code ends here*/
//	while(1)
	while(0)
	{
	    memset(UDP_Packet, '\0', sizeof(UDP_Packet));
		gLength = recvfrom(sockfd1, UDP_Packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *)&addr, &addrlen);
		if (toggle_val == 1)
		{
	    	memset(gUDP_Packet, '\0', sizeof(gUDP_Packet));
			strcpy(gUDP_Packet, UDP_Packet);
//			fprintf(stderr,"switch to backup reference\n");
	        //gLength = recvfrom(sockfd1, gUDP_Packet, UDP_MAXIMUM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addrlen);
			//fprintf(stderr,"b\t");
		}
	}
	printf("TEST 4\n");
}

void *tsReference(void *Data)
{
    int LanguageId = 0;
    LanguageId = *((int *)Data);
    
    if(LanguageId > 0)
    {
        LanguageId = LanguageId - 1;
    }
    //DEBUG_PRINTF("Ref:Offset[%f] myDataIP[%s] myDataPort[%s] myPid[%d] operflag[%d]\n", myData->offSet, myData->IP, myData->PORT_P, *myData->PID, myData->oprFlag);
    DEBUG_PRINTF("Reference Thread started, instance[%d]\n", LanguageId);
    fprintf(stderr,"Reference Thread started, instance[%d]\n", LanguageId);
    unsigned char res, res1, res2;
    unsigned char packet[PACKET_SIZE];
    unsigned short pid_id; /**this variable will keep information of PID of 13 bit*/
    char payLoadStartIndi;
    int idx;
#if 1
    int sockfd = 0;
    int sockfd1 = 0;
    int len;
    struct sockaddr_in addr;
#ifdef HAVE_IP_MREQN
    struct ip_mreqn mgroup;
#else
    struct ip_mreq mgroup;
#endif
    int reuse, reuse1;
    unsigned int addrlen;
//    int len;
    unsigned char udp_packet[UDP_MAXIMUM_SIZE];

    memset((char *)&mgroup, 0, sizeof(mgroup));
    mgroup.imr_multiaddr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].ReferenceIp);
#ifdef HAVE_IP_MREQN
    mgroup.imr_address.s_addr = INADDR_ANY;
#else
    mgroup.imr_interface.s_addr = INADDR_ANY;
#endif
    memset((char *)&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(inputArgsAllLanguages[LanguageId].ReferencePort);
    addr.sin_addr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].ReferenceIp);
    addrlen = sizeof(addr);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("socket(): error ");
        return 0;
    }
    reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt() SO_REUSEADDR: error ");
    }

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind(): error");
        close(sockfd);
        return 0;
    }

    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mgroup, sizeof(mgroup)) < 0)
    {
        perror("setsockopt() IPPROTO_IP: error ");
        close(sockfd);
        return 0;
    }
#endif
#if 0
/*Code for backup socket*/
	memset((char *)&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(inputArgsAllLanguages[LanguageId].BackupReferencePort);
	addr.sin_addr.s_addr = inet_addr(inputArgsAllLanguages[LanguageId].BackupReferenceIp);
	addrlen = sizeof(addr);
	sockfd1 = socket(AF_INET, SOCK_DGRAM, 0);
	printf("TEST 1\n");
	if (sockfd1 < 0)
	{
			perror("socket(): error ");
			return 0;
	}
	reuse1 = 1;
	printf("TEST 2\n");
	if (setsockopt(sockfd1, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse1, sizeof(reuse1)) < 0)
	{
			perror("setsockopt() SO_REUSEADDR: error ");
	}
	printf("TEST 3\n");
	if (bind(sockfd1, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
			perror("bind(): error");
			close(sockfd1);
			return 0;
	}
	printf("TEST 4\n");
	if (setsockopt(sockfd1, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mgroup, sizeof(mgroup)) < 0)
	{
			perror("setsockopt() IPPROTO_IP: error ");
			close(sockfd1);
			return 0;
	}
	/*Code ends here*/
#endif

    //while (1)
    while (thread_is_live)
    {
	    memset(udp_packet, '\0', sizeof(udp_packet));
        len = recvfrom(sockfd, udp_packet, UDP_MAXIMUM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addrlen);
    #if 0
	    memset(udp_packet, '\0', sizeof(udp_packet));
		if (toggle_val == 0)
		{
			fprintf(stderr,"tsRefAutoSwitchTimeStarts before reset\n");
			tsRefAutoSwitchTimeStarts = clock();
			//len = recvfrom(sockfd, udp_packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *)&addr, &addrlen);
            len = recvfrom(sockfd, udp_packet, UDP_MAXIMUM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addrlen);
			tsRefAutoSwitchTimeStarts = clock();
			/*printf("value of len: %d\t",len);*/
			fprintf(stderr,"tsRefAutoSwitchTimeStarts reset\n");
		}
		if (toggle_val == 1)
		{
			fprintf(stderr,"switch to backup reference\n");
			//len = recvfrom(sockfd1, udp_packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *)&addr, &addrlen);
            len = recvfrom(sockfd1, udp_packet, UDP_MAXIMUM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addrlen);
		}
	#endif
		//fprintf(stderr,"TEST 7, gLength : %d\n", gLength);

        //if (gLength == UDP_MAXIMUM_SIZE) /**here we can use len must be equal to return */
        if (len == UDP_MAXIMUM_SIZE) /**here we can use len must be equal to return */
        {

            for (idx = 0; idx < UDP_MAXIMUM_SIZE; idx = idx + PACKET_SIZE) //loop for break up packet
            {
                memset(packet, '\0', sizeof(packet));
                //memcpy(packet, gUDP_Packet + idx, PACKET_SIZE); //copy 188 byte memory in to chunk
                memcpy(packet, udp_packet + idx, PACKET_SIZE); //copy 188 byte memory in to chunk
                payLoadStartIndi = packet[1] & 0x1f;
                pid_id = payLoadStartIndi << 8;
                pid_id = pid_id | packet[2];
                /* sync byte = 0x47 and pid = 0x045c matched*/
                if ((packet[0] == 0x47) && (inPidList(pid_id, inputArgsAllLanguages[LanguageId].ReferencePID)))
                {
                    payLoadStartIndi = packet[1] & 0x40;
                    /*payload stat indicator bit set*/
                    if (payLoadStartIndi == 0x40)
                    {
                        res2 = packet[3] & 0x30;
                        if ((res2 == 0x10)) // check for adaption field
                        {
                            unsigned char psc1, psc2, psc3;
                            psc1 = packet[4]; //      PES
                            psc2 = packet[5]; //      START
                            psc3 = packet[6]; //      Indicator
							//psc4 = packet[7];
                                              //	printf("TEST 3b\t");
                            /**packet elementary  start code**/
                            if ((psc1 == 0x00) && (psc2 == 0x00) && (psc3 == 0x01))
                            {
                                res1 = packet[11] & 0xc0;
                                res2 = packet[11] & 0x80;
                                /**0x80 means only pts present and 0xc0 means ptsand dts both info present*/
                                if ((res1 == 0xc0) || (res2 == 0x80))
                                {
                                    res = packet[13] & 0xf0;
                                    if ((res == 0x20) || (res == 0x30))
                                    {
                                        /**here we are calculating 33 pts info only*/
                                        unsigned long long pts1, pts2, pts3, pts_m, pts = 0;
                                        pts1 = (packet[13] & 0x0f) >> 1;
                                        pts2 = ((packet[14] << 8) | packet[15]) >> 1;
                                        pts3 = ((packet[16] << 8) | packet[17]) >> 1;
                                        pts = (pts1 << 30) | (pts2 << 15) | pts3;
                                        pts_m = pts + ((inputArgsAllLanguages[LanguageId].Offset) * 90000);
                                        //fprintf(stderr, "\npts_m1 : %llu\n\n", pts_m);
                                        //pts_m = pts + ((0) * 90000);
                                        if(LanguageId >= 0)
                                        {
                                            nPTS[LanguageId] =  pts_m;
                                        }
                                        else
                                        {
                                            DEBUG_PRINTF("\nIncorrect language id in reference : %d\n", LanguageId);
                                        }
                                    } /*end of condition to check pts  is present or not**/
                                }     /**end of condition of packet elementary code*/
                            }
                        }
                        else if (res2 == 0x30) // check for adaption field
                        {
                            int dataLen = packet[4];          //sizeOf data packet
                            int startIndicator = dataLen + 5; //finds start of PES
                            unsigned char psc1, psc2, psc3;
                            psc1 = packet[startIndicator];   //      PES
                            psc2 = packet[++startIndicator]; //      START
                            psc3 = packet[++startIndicator]; //      Indicator
							//psc4 = packet[++startIndicator]; //      stream ID
							++startIndicator;
                            /**packet elementery start code**/
                            if ((psc1 == 0x00) && (psc2 == 0x00) && (psc3 == 0x01))
                            {
                                res1 = packet[startIndicator + 4] & 0xc0;
                                res2 = packet[startIndicator + 4] & 0x80;
                                /**0x80 means only pts present and 0xc0 means pts and dts both info present*/
                                if ((res1 == 0xc0) || (res2 == 0x80))
                                {
                                    res = packet[startIndicator + 6] & 0xf0;
                                    if ((res == 0x20) || (res == 0x30))
                                    {
                                        /**here we are calculating 33 pts info only*/
                                        unsigned long long pts1, pts2, pts3, pts_m, pts = 0;
                                        pts1 = (packet[startIndicator + 6] & 0x0f) >> 1;
                                        pts2 = ((packet[startIndicator + 7] << 8) | packet[startIndicator + 8]) >> 1;
                                        pts3 = ((packet[startIndicator + 9] << 8) | packet[startIndicator + 10]) >> 1;
                                        pts = (pts1 << 30) | (pts2 << 15) | pts3;
                                        pts_m = pts + ((inputArgsAllLanguages[LanguageId].Offset) * 90000);
                                        //fprintf(stderr, "\npts_m1 : %llu\n\n", pts_m);
                                        if(LanguageId >= 0)
                                        {
                                            nPTS[LanguageId] = pts_m;
                                        }
                                        else
                                        {
                                            DEBUG_PRINTF("\nIncorrect language id in reference:%d\n", LanguageId);
                                        }
                                    }
                                }
                            } /**end of condition of packet elementary code*/
                        } /**adaptation field*/
                    }
                } /**end of sync byte condition*/
            }/**end of for loop*/
        }/**end of length check 1316*/
    }/**end of while loop*/
    close(sockfd);
}

/***************************************************************TSUDPSEND***************************************************************************/
long long int usecDiff(struct timeval* time_stop, struct timeval* time_start)
{
	long long int temp = 0;
	long long int utemp = 0;
		   
	if (time_stop && time_start) {
		if (time_stop->tv_usec >= time_start->tv_usec) {
			utemp = time_stop->tv_usec - time_start->tv_usec;    
			temp = time_stop->tv_sec - time_start->tv_sec;
		} else {
			utemp = time_stop->tv_usec + 1000000 - time_start->tv_usec;       
			temp = time_stop->tv_sec - 1 - time_start->tv_sec;
		}
		if (temp >= 0 && utemp >= 0) {
			temp = (temp * 1000000) + utemp;
        	} else {
			fprintf(stderr, "start time %ld.%ld is after stop time %ld.%ld\n", time_start->tv_sec, time_start->tv_usec, time_stop->tv_sec, time_stop->tv_usec);
			temp = -1;
		}
	} else {
		fprintf(stderr, "memory is garbaged?\n");
		temp = -1;
	}
        return temp;
}

#if 1
//int tsudpsend(int argc, char *argv[]) {
void *tsudpsend(void *Data) {
    
    int Language_id = *(int *)Data;
    int sockfd;
    int len;
    int sent;
    int transport_fd;
    struct sockaddr_in addr;
    unsigned long int packet_size;   
    char tsfile[100];
    unsigned char* send_buf;
    unsigned int bitrate;
    unsigned long long int packet_time;
    unsigned long long int real_time;    
    struct timeval time_start;
    struct timeval time_stop;
    struct timespec nano_sleep_packet;
    
    memset(&addr, 0, sizeof(addr));
    memset(&time_start, 0, sizeof(time_start));
    memset(&time_stop, 0, sizeof(time_stop));
    memset(&nano_sleep_packet, 0, sizeof(nano_sleep_packet));
    fprintf(stderr, "Output thread started for [%d] port\n", inputArgs->PortOutputToMux + Language_id - 1);
/*
    if(argc < 5 ) {
	fprintf(stderr, "Usage: %s file.ts ipaddr port bitrate [ts_packet_per_ip_packet]\n", argv[0]);
	fprintf(stderr, "ts_packet_per_ip_packet default is 7\n");
	fprintf(stderr, "bit rate refers to transport stream bit rate\n");
	fprintf(stderr, "zero bitrate is 100.000.000 bps\n");
	return 0;
    } else {*/
	//tsfile = argv[1];
    memset(tsfile, '\0', sizeof(tsfile));
    sprintf(tsfile,"test_%d.ts", Language_id);
	addr.sin_family = AF_INET;
//	addr.sin_addr.s_addr = inet_addr("224.1.1.2");
	addr.sin_addr.s_addr = inet_addr(inputArgs->IPOutputToMux);
	addr.sin_port = htons(inputArgs->PortOutputToMux + Language_id - 1);
	bitrate = 5000000;
	if (bitrate <= 0) {
	    bitrate = 100000000;
	}
	/*if (argc >= 6) {
	    packet_size = strtoul(argv[5], 0, 0) * TS_PACKET_SIZE;
	} else {
	  */  packet_size = 7 * TS_PACKET_SIZE;
	/*}
    }*/

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0) {
	    perror("socket(): error ");
	    return 0;
    } 
    
    transport_fd = open(tsfile, O_RDONLY);
    if(transport_fd < 0) {
	    fprintf(stderr, "can't open file %s\n", tsfile);
	    close(sockfd);
	    return 0;
    }
    else
    {
        fprintf(stderr, "File opened : [%s], with fd : [%d]\n", tsfile, transport_fd);
    }
    
    int completed = 0;
    send_buf = malloc(packet_size);
    packet_time = 0;
    real_time = 0;
//sleep(5);
    gettimeofday(&time_start, 0);
    //while (!completed) {
    while (!completed && thread_is_live) 
    {
	    gettimeofday(&time_stop, 0);
	    real_time = usecDiff(&time_stop, &time_start);
	    if (real_time * bitrate > packet_time * 1000000) 
        { /* theorical bits against sent bits */
		    len = read(transport_fd, send_buf, packet_size);
		    if(len < 0) 
            {
		        fprintf(stderr, "ts file read error \n");
		        completed = 1;
		    } 
            else if (len == 0) 
            {
	    	    //fprintf(stderr, "ts sent done\n");	    
                //sleep(2);
                continue;
                //    completed = 1;
		    } 
            else 
            {
		        sent = sendto(sockfd, send_buf, len, 0, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		        if(sent <= 0) 
                {
			        perror("send(): error ");
			        completed = 1;
		        } 
                else 
                {
			        packet_time += packet_size * 8;
                    fprintf(stderr, "Output packet sent on : %d\r", inputArgs->PortOutputToMux + Language_id - 1);
		        }
		    }
	    } 
        else 
        {
            fprintf(stderr, "Going to nanosleep************\r");
    		nanosleep(&nano_sleep_packet, 0);
	    }
    }

    fprintf(stderr, "End of tsudpsend************\n");
    close(transport_fd);
    close(sockfd);
    free(send_buf);
    return 0;    
}
#endif
/*************************************************************************************************************************************************/

void *tsReplace(void *Data)
{
    char file_name[MAXDATA];
    int Language_id = *(int *)Data;
    //struct data *myData;
    //myData = (struct data *)Data;
    if(Language_id > 0)
    {
        Language_id = Language_id - 1;
    }
    memset(file_name, '\0', sizeof(file_name));
    sprintf(file_name, "test_%d.ts", Language_id + 1);
    int fd1 = open(file_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    DEBUG_PRINTF("Replace thread started, instance:%d\n", Language_id);
    unsigned char res, res1, res2, res3;
    unsigned char packet[PACKET_SIZE];
    
    unsigned char packet_1316[1316];//Added on 01072022
    int pkt_creator = 0; 
    unsigned short pid_id; /**this variable will keep information of PID of 13 bit*/
    char payLoadStartIndi;
    //unsigned long long int nPTS = 0;

    int sockfd = 0;
    struct sockaddr_in addr;

/******************************TSUDPSEND******************************
    int tsudpsend_sockfd;
    int tsudpsend_len;
    int sent;
    struct sockaddr_in tsudpsend_addr;
    unsigned long int packet_size;   
    unsigned int bitrate;
    unsigned long long int packet_time;
    unsigned long long int real_time;    
    struct timeval time_start;
    struct timeval time_stop;
    struct timespec nano_sleep_packet;
    
    memset(&addr, 0, sizeof(addr));
    memset(&time_start, 0, sizeof(time_start));
    memset(&time_stop, 0, sizeof(time_stop));
    memset(&nano_sleep_packet, 0, sizeof(nano_sleep_packet));
******************************TSUDPSEND******************************/

#ifdef HAVE_IP_MREQN
    struct ip_mreqn mgroup;
#else
    struct ip_mreq mgroup;
#endif
    int reuse;
    unsigned int addrlen;
    int len, idx;
    unsigned char udp_packet[UDP_MAXIMUM_SIZE];

    memset((char *)&mgroup, 0, sizeof(mgroup));
    mgroup.imr_multiaddr.s_addr = inet_addr("127.0.0.1");
#ifdef HAVE_IP_MREQN
    mgroup.imr_address.s_addr = INADDR_ANY;
#else
    mgroup.imr_interface.s_addr = INADDR_ANY;
#endif
    memset((char *)&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8210 + Language_id + 1);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addrlen = sizeof(addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("socket(): error ");
        return 0;
    }

    reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt() SO_REUSEADDR: error ");
    }

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind(): error");
        close(sockfd);
        return 0;
    }

    fprintf(stderr, "Language_id:%d,thread is live Replace : %d\n",Language_id, thread_is_live);

    //while (1)
    while (thread_is_live)
    {
        memset(udp_packet, '\0', sizeof(udp_packet));
        len = recvfrom(sockfd, udp_packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *)&addr, &addrlen);

        if (len == UDP_MAXIMUM_SIZE) /**here we can use len must be equal to return */
        {
            for (idx = 0; idx < UDP_MAXIMUM_SIZE; idx = idx + PACKET_SIZE) //loop for break up packet
            {
                memset(packet, '\0', sizeof(packet));
                memcpy(packet, udp_packet + idx, PACKET_SIZE); //copy 188 byte memory in to chunk
                payLoadStartIndi = packet[1] & 0x1f;
                pid_id = payLoadStartIndi << 8;
                pid_id = pid_id | packet[2];
				//fprintf(stderr,"pid : %d, LangPID : %d\n", pid_id, inputArgsAllLanguages[Language_id].ReplacePID);
                if ((packet[0] == 0x47) && (inPidList(pid_id, inputArgsAllLanguages[Language_id].ReplacePID)))
                {
	//	            fprintf(stderr,"value of pid_id: %d || value of myData->PID: %d, language id : %d, lang : [%s]\n",pid_id,inputArgsAllLanguages[Language_id].ReplacePID, Language_id, inputArgsAllLanguages[Language_id].Language);
                    payLoadStartIndi = packet[1] & 0x40;
                    /*payload stat indicator bit set*/
                    if (payLoadStartIndi == 0x40)
                    {
                        res3 = packet[3] & 0x30;
                        if (res3 == 0x10) // check for adaption field
                        {
                            unsigned char psc1, psc2, psc3;
                            psc1 = packet[4]; //      PES
                            psc2 = packet[5]; //      START
                            psc3 = packet[6]; //      Indicator
							//psc4 = packet[7]; //      stream ID
                            /**packet elementary start code**/
							DEBUG_PRINTF("TEST 1 for : [%s] \t\t",file_name);
                            if ((psc1 == 0x00) && (psc2 == 0x00) && (psc3 == 0x01))
                            {
                                res1 = packet[11] & 0xc0;
                                res2 = packet[11] & 0x80;
                                /**0x80 means only pts present and 0xc0 means pts and dts both info present*/
								DEBUG_PRINTF("TEST 2 for : [%s] \t\t",file_name);
                                if ((res1 == 0xc0) || (res2 == 0x80))
                                {
                                    res = packet[13] & 0xf0;	
									DEBUG_PRINTF("TEST 3 for : [%s] \t\t",file_name);
                                    if ((res == 0x20) || (res == 0x30))
                                    {
                                        #ifdef DEBUG
                                        /**here we are calculating 33 pts info only*/
                                        unsigned long long pts1, pts2, pts3, pts = 0;
                                        pts1 = (packet[13] & 0x0f) >> 1;
                                        pts2 = ((packet[14] << 8) | packet[15]) >> 1;
                                        pts3 = ((packet[16] << 8) | packet[17]) >> 1;
                                        pts = (pts1 << 30) | (pts2 << 15) | pts3;
                                            long int time = pts / 90000;
                                            long int mymSec = (pts % 90000) / 9;
                                            long int ntime = nPTS[Language_id] / 90000;
                                            long int nmymSec = (nPTS[Language_id] % 90000) / 9;
                                        #endif
                                        DEBUG_PRINTF("\rRef.PTS:%s.%ld-->%s.%ld", secToTime(ntime), nmymSec, secToTime(time), mymSec);
                                        DEBUG_PRINTF("Replacing for PID [%d]\n", pid_id);
                                        DEBUG_PRINTF("\tRep npts[%d][%llu]\n",__LINE__,nPTS[Language_id]);
                                        changePts(nPTS[Language_id], &packet[0], 0);
                                    }
                                }
                            }

                        }                      /**adaptation field*/
                        else if (res3 == 0x30) // check for adaption field
                        {
                            int dataLen = packet[4];          //sizeOf data packet
                            int startIndicator = dataLen + 5; //finds start of PES
                            unsigned char psc1, psc2, psc3;
                            psc1 = packet[startIndicator];   //      PES
                            psc2 = packet[++startIndicator]; //      START
                            psc3 = packet[++startIndicator]; //      Indicator
                            //psc4 = packet[++startIndicator]; //      stream ID
                            ++startIndicator;
                            /**packet elementery start code**/
                            if ((psc1 == 0x00) && (psc2 == 0x00) && (psc3 == 0x01))
                            {
                                res1 = packet[startIndicator + 4] & 0xc0;
                                res2 = packet[startIndicator + 4] & 0x80;
                                /**0x80 means only pts present and 0xc0 means pts and dts both info present*/
                                if ((res1 == 0xc0) || (res2 == 0x80))
                                {
                                    res = packet[startIndicator + 6] & 0xf0;
                                    if ((res == 0x20) || (res == 0x30))
                                    {
                                        #ifdef DEBUG
                                        /**here we are calculating 33 pts info only*/
                                        unsigned long long pts1, pts2, pts3, pts = 0;
                                        pts1 = (packet[startIndicator + 6] & 0x0f) >> 1;
                                        pts2 = ((packet[startIndicator + 7] << 8) | packet[startIndicator + 8]) >> 1;
                                        pts3 = ((packet[startIndicator + 9] << 8) | packet[startIndicator + 10]) >> 1;
                                        pts = (pts1 << 30) | (pts2 << 15) | pts3;
                                            long int time = pts / 90000;
                                            long int mymSec = (pts % 90000) / 9;
                                            /*long int ntime = nPTS[Language_id - 1] / 90000;
                                            long int nmymSec = (nPTS[Language_id - 1] % 90000) / 9;*/
                                            
                                            long int ntime = nPTS[Language_id] / 90000;
                                            long int nmymSec = (nPTS[Language_id] % 90000) / 9;
                                        #endif
                                        DEBUG_PRINTF("\tReplacing for PID [%d]\n", pid_id);
                                        DEBUG_PRINTF("\rRef.PTS:%s.%ld-->%s.%ld, file : [%s]", secToTime(ntime), nmymSec, secToTime(time), mymSec,file_name);
                                        DEBUG_PRINTF("\tRep npts[%d][%llu]\n",__LINE__,nPTS[Language_id]);
                                        changePts(nPTS[Language_id], &packet[0], startIndicator);
                                        DEBUG_PRINTF(" %d receive continuity_counter[%x]\n",__LINE__,0x0f & packet[3]);    
                                    }
                                }
                            } /**end of condition of packet elementary code*/

                        } /**adaptation field*/

                    } /**payload start indicator*/

                } /**sync byte*/
                /*This code will write the data on file*/
                if (write(fd1, &packet[0], PACKET_SIZE) != PACKET_SIZE)
                {
                    fprintf(stderr, "%d write error\n", __LINE__);
                }
      //          fprintf(stderr, "Output packet sent on : %d\r", inputArgs->PortOutputToMux + Language_id - 1);
                /*write code is repalce by tsudpsend code so that packet can be sent to udp socket*/
            } /**end of for loop*/
        }     /**end of length check 1316*/
    }         /**end of while loop*/
    fprintf(stderr, "1Exiting from tsReplace\n");
    /*close(sockfd);
    fclose(fd1);
    fprintf(stderr, "2Exiting from tsReplace\n");*/
}

/*****************************************************************************
 * Signal Handler
 *****************************************************************************/
static void SigHandler(int i_signal)
{
    b_die = b_error = 1;
}

/*****************************************************************************
 * Poll: factorize polling code
 *****************************************************************************/
static bool Poll(int i_input_fd)
{
    struct pollfd pfd;
    int i_ret;

    pfd.fd = i_input_fd;
    pfd.events = POLLIN | POLLERR | POLLRDHUP | POLLHUP;

    i_ret = poll(&pfd, 1, POLL_TIMEOUT);
    if (i_ret < 0)
    {
        msg_Err(NULL, "poll error (%s)", strerror(errno));
        b_die = b_error = 1;
        return false;
    }
    if (pfd.revents & (POLLERR | POLLRDHUP | POLLHUP))
    {
        msg_Err(NULL, "poll error");
        b_die = b_error = 1;
        return false;
    }
    if (!i_ret)
        return false;

    return true;
}

/*****************************************************************************
 * tcp_*: TCP socket handlers (only what differs from UDP)
 *****************************************************************************/
static uint8_t *p_tcp_buffer = NULL;
static size_t i_tcp_size = 0;

static ssize_t tcp_Read(void *p_buf, size_t i_len, int i_input_fd)
{
    if (p_tcp_buffer == NULL)
        p_tcp_buffer = malloc(i_len);

    uint8_t *p_read_buffer;
    ssize_t i_read_size = i_len;
    p_read_buffer = p_tcp_buffer + i_tcp_size;
    i_read_size -= i_tcp_size;

    if ((i_read_size = recv(i_input_fd, p_read_buffer, i_read_size, 0)) < 0)
    {
        msg_Err(NULL, "recv error (%s)", strerror(errno));
        b_die = b_error = 1;
        return 0;
    }

    i_tcp_size += i_read_size;
    i_stc = pf_Date();

    if (i_tcp_size != i_len)
        return 0;

    memcpy(p_buf, p_tcp_buffer, i_len);
    i_tcp_size = 0;
    return i_len;
}

/*****************************************************************************
 * udp_*: UDP socket handlers
 *****************************************************************************/
static off_t i_udp_nb_skips = 0;
static bool b_tcp = false;

static ssize_t udp_Read(void *p_buf, size_t i_len, FILE *p_input_aux, int i_input_fd)
{
    ssize_t i_ret;
    if (!i_udp_nb_skips && !i_first_stc)
        i_first_stc = pf_Date();

    if (!Poll(i_input_fd))
    {
        i_stc = pf_Date();
        return 0;
    }

    if (!b_tcp)
    {
        if ((i_ret = recv(i_input_fd, p_buf, i_len, 0)) < 0)
        {
            msg_Err(NULL, "recv error (%s)", strerror(errno));
            b_die = b_error = 1;
            return 0;
        }

#ifdef HAVE_TIMESTAMPS
        struct timespec ts;
        if (!ioctl(i_input_fd, SIOCGSTAMPNS, &ts))
            i_stc = ts.tv_sec * UINT64_C(27000000) + ts.tv_nsec * 27 / 1000;
        else
#endif
            i_stc = pf_Date();
    }
    else
        i_ret = tcp_Read(p_buf, i_len, i_input_fd);

    if (i_udp_nb_skips)
    {
        i_udp_nb_skips--;
        return 0;
    }
    return i_ret;
}

static void udp_ExitRead(int i_input_fd)
{
    close(i_input_fd);
    if (p_tcp_buffer != NULL)
        free(p_tcp_buffer);
}

static int udp_InitRead(const char *psz_arg, size_t i_len,
                        off_t i_nb_skipped_chunks, int64_t i_pos, int *i_input_fd)
{
    if (i_pos || (*i_input_fd = OpenSocket(psz_arg, i_ttl, DEFAULT_PORT, 0,
                                          NULL, &b_tcp, NULL)) < 0)
        return -1;

    i_udp_nb_skips = i_nb_skipped_chunks;

    pf_Read = udp_Read;
    pf_ExitRead = udp_ExitRead;
#ifdef HAVE_TIMESTAMPS
    if (!b_tcp)
        pf_Date = real_Date;
#endif
    return 0;
}

static ssize_t raw_Write(const void *p_buf, size_t i_len, int i_output_fd)
{
#ifndef __APPLE__
    ssize_t i_ret;
    struct iovec iov[2];

#if defined(__FreeBSD__)
    pktheader.udph.uh_ulen
#else
    pktheader.udph.len
#endif
        = htons(sizeof(struct udphdr) + i_len);

#if defined(__FreeBSD__)
    pktheader.iph.ip_len = htons(sizeof(struct udprawpkt) + i_len);
#endif

    iov[0].iov_base = &pktheader;
    iov[0].iov_len = sizeof(struct udprawpkt);

    iov[1].iov_base = (void *)p_buf;
    iov[1].iov_len = i_len;

    if ((i_ret = writev(i_output_fd, iov, 2)) < 0)
    {
        if (errno == EBADF || errno == ECONNRESET || errno == EPIPE)
        {
            msg_Err(NULL, "write error (%s)", strerror(errno));
            b_die = b_error = 1;
        }
        /* otherwise do not set b_die because these errors can be transient */
        return 0;
    }

    return i_ret;
#else
    return -1;
#endif
}

/* Please note that the write functions also work for TCP */
static ssize_t udp_Write(const void *p_buf, size_t i_len, int i_output_fd)
{
    ssize_t i_ret;
    if ((i_ret = send(i_output_fd, p_buf, i_len, 0)) < 0)
    {
        if (errno == EBADF || errno == ECONNRESET || errno == EPIPE)
        {
            msg_Err(NULL, "write error (%s)", strerror(errno));
            b_die = b_error = 1;
        }
        /* otherwise do not set b_die because these errors can be transient */
        return 0;
    }

    return i_ret;
}

static void udp_ExitWrite(int i_output_fd)
{
    close(i_output_fd);
}

static int udp_InitWrite(const char *psz_arg, size_t i_len, bool b_append, int *i_output_fd)
{
    fprintf(stderr, "First argument : [%s]\n", psz_arg);
    struct opensocket_opt opt;
    memset(&opt, 0, sizeof(struct opensocket_opt));
    if (b_raw_packets)
    {
        opt.p_raw_pktheader = &pktheader;
    }

    if ((*i_output_fd = OpenSocket(psz_arg, i_ttl, 0, DEFAULT_PORT,
                                  NULL, NULL, &opt)) < 0)
    {
        fprintf(stderr, "Socket can't open\t\t");
        return -1;
    }
    if (b_raw_packets)
    {
        DEBUG_PRINTF("raw_packet \t\t");
        pf_Write = raw_Write;
    }
    else
    {
        DEBUG_PRINTF("udp_packet \t\t");
        pf_Write = udp_Write;
    }
    pf_ExitWrite = udp_ExitWrite;
    return 0;
}

/*****************************************************************************
 * stream_*: FIFO and character device handlers
 *****************************************************************************/
static off_t i_stream_nb_skips = 0;
static ssize_t i_buf_offset = 0;

static ssize_t stream_Read(void *p_buf, size_t i_len, FILE *p_input_aux, int i_input_fd)
{
    ssize_t i_ret;
    if (!i_stream_nb_skips && !i_first_stc)
        i_first_stc = pf_Date();

    if (!Poll(i_input_fd))
    {
        i_stc = pf_Date();
        return 0;
    }

    if ((i_ret = read(i_input_fd, p_buf + i_buf_offset,
                      i_len - i_buf_offset)) < 0)
    {
        msg_Err(NULL, "read error (%s)", strerror(errno));
        b_die = b_error = 1;
        return 0;
    }

    i_stc = pf_Date();
    i_buf_offset += i_ret;

    if (i_buf_offset < i_len)
        return 0;

    i_ret = i_buf_offset;
    i_buf_offset = 0;
    if (i_stream_nb_skips)
    {
        i_stream_nb_skips--;
        return 0;
    }
    return i_ret;
}

static void stream_ExitRead(int i_input_fd)
{
    close(i_input_fd);
}

static int stream_InitRead(const char *psz_arg, size_t i_len,
                           off_t i_nb_skipped_chunks, int64_t i_pos, int *i_input_fd)
{
    if (i_pos)
        return -1;

    *i_input_fd = OpenFile(psz_arg, true, false);
    if (*i_input_fd < 0)
        return -1;
    i_stream_nb_skips = i_nb_skipped_chunks;

    pf_Read = stream_Read;
    pf_ExitRead = stream_ExitRead;
    return 0;
}

static ssize_t stream_Write(const void *p_buf, size_t i_len, int i_output_fd)
{
    ssize_t i_ret;
retry:
    if ((i_ret = write(i_output_fd, p_buf, i_len)) < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
            goto retry;
        msg_Err(NULL, "write error (%s)", strerror(errno));
        b_die = b_error = 1;
    }
    return i_ret;
}

static void stream_ExitWrite(int i_output_fd)
{
    close(i_output_fd);
}

static int stream_InitWrite(const char *psz_arg, size_t i_len, bool b_append, int *i_output_fd)
{
    *i_output_fd = OpenFile(psz_arg, false, b_append);
    if (*i_output_fd < 0)
        return -1;

    pf_Write = stream_Write;
    pf_ExitWrite = stream_ExitWrite;
    return 0;
}

/*****************************************************************************
 * file_*: handler for the auxiliary file format
 *****************************************************************************/
static uint64_t i_file_next_flush = 0;

static ssize_t file_Read(void *p_buf, size_t i_len, FILE *p_input_aux, int i_input_fd)
{
    uint8_t p_aux[8];
    ssize_t i_ret;

    if ((i_ret = read(i_input_fd, p_buf, i_len)) < 0)
    {
        msg_Err(NULL, "Function : %s giving read error (%s)", __func__, strerror(errno));
        b_die = b_error = 1;
        return 0;
    }
    if (i_ret == 0)
    {
        msg_Dbg(NULL, "end of file reached");
        b_die = 1;
        thread_is_live = 0;//Added on 7aug
        return 0;
    }

    if(p_input_aux == NULL)
    {
        DEBUG_PRINTF("p_input_aux is not initialized [Error:1003]\nExiting!!!\n");
        exit(0);
    }
    if (fread(p_aux, 8, 1, p_input_aux) != 1)
    {
        msg_Warn(NULL, "premature end of aux file reached");
        b_die = b_error = 1;
        return 0;
    }
    i_stc = FromSTC(p_aux);
    if (!i_first_stc)
        i_first_stc = i_stc;

    return i_ret;
}

static bool file_Delay(void)
{
    /* for correct throughput without rounding approximations */
    static uint64_t i_file_first_stc = 0, i_file_first_wall = 0;
    uint64_t i_wall = pf_Date();

    if (!i_file_first_wall)
    {
        i_file_first_wall = i_wall;
        i_file_first_stc = i_stc;
    }
    else
    {
        int64_t i_delay = (i_stc - i_file_first_stc) -
                          (i_wall - i_file_first_wall);
        if (i_delay > 0)
            pf_Sleep(i_delay);
        else if (i_delay < -MAX_LATENESS)
        {
            msg_Warn(NULL, "too much lateness, resetting clocks");
            i_file_first_wall = i_wall;
            i_file_first_stc = i_stc;
        }
    }
    return true;
}

static void file_ExitRead(int i_input_fd)
{
    close(i_input_fd);
}

static int file_InitRead(const char *psz_arg, size_t i_len,
                         off_t i_nb_skipped_chunks, int64_t i_pos, FILE **p_input_aux, int *i_input_fd)
{
    char *psz_aux_file = GetAuxFile(psz_arg, i_len);

    if (i_pos)
    {
        i_nb_skipped_chunks = LookupAuxFile(psz_aux_file, i_pos, false);
        if (i_nb_skipped_chunks < 0)
        {
            free(psz_aux_file);
            return -1;
        }
    }

    *i_input_fd = OpenFile(psz_arg, true, false);
    fprintf(stderr, "Line:%d, psz_arg : [%s]\n",__LINE__, psz_arg);
    if (*i_input_fd < 0)
    {
        DEBUG_PRINTF("print 2 Openfile exit\n");
        free(psz_aux_file);
        return -1;
    }
    
    *p_input_aux = OpenAuxFile(psz_aux_file, true, false);
    free(psz_aux_file);
    if (*p_input_aux == NULL)
    {   
        DEBUG_PRINTF("\np_input_aur is not initialized [Error:1002], Exiting!!!\n");
        return -1;
    }
    DEBUG_PRINTF("Function : [%s], value of p_input_aux : %p\n", __func__, *p_input_aux);
    lseek(*i_input_fd, (off_t)i_len * i_nb_skipped_chunks, SEEK_SET);
    fseeko(*p_input_aux, 8 * i_nb_skipped_chunks, SEEK_SET);

    pf_Read = file_Read;
    pf_Delay = file_Delay;
    pf_ExitRead = file_ExitRead;
    return 0;
}

static ssize_t file_Write(const void *p_buf, size_t i_len, int i_output_fd)
{
    uint8_t p_aux[8];
    ssize_t i_ret;
#ifdef DEBUG_WRITEBACK
    uint64_t start = pf_Date(), end;
#endif

    if ((i_ret = write(i_output_fd, p_buf, i_len)) < 0)
    {
        msg_Err(NULL, "couldn't write to file (%s)", strerror(errno));
        b_die = b_error = 1;
        return i_ret;
    }
#ifdef DEBUG_WRITEBACK
    end = pf_Date();
    if (end - start > 270000) /* 10 ms */
        msg_Err(NULL, "too long waiting in write(%" PRId64 ")", (end - start) / 27000);
#endif

    ToSTC(p_aux, i_stc);
    if (fwrite(p_aux, 8, 1, p_output_aux) != 1)
    {
        msg_Err(NULL, "couldn't write to auxiliary file");
        b_die = b_error = 1;
    }
    if (!i_file_next_flush)
        i_file_next_flush = i_stc + FILE_FLUSH;
    else if (i_file_next_flush <= i_stc)
    {
        fflush(p_output_aux);
        i_file_next_flush = i_stc + FILE_FLUSH;
    }

    return i_ret;
}

static void file_ExitWrite(int i_output_fd)
{
    close(i_output_fd);
    fclose(p_output_aux);
}

static int file_InitWrite(const char *psz_arg, size_t i_len, bool b_append, int *i_output_fd)
{
    DEBUG_PRINTF("test print in file_InitWrite \t\t");
    char *psz_aux_file = GetAuxFile(psz_arg, i_len);
    if (b_append)
        CheckFileSizes(psz_arg, psz_aux_file, i_len);
    *i_output_fd = OpenFile(psz_arg, false, b_append);
    if (i_output_fd < 0)
        return -1;
    p_output_aux = OpenAuxFile(psz_aux_file, false, b_append);
    free(psz_aux_file);
    if (p_output_aux == NULL)
        return -1;

    pf_Write = file_Write;
    pf_ExitWrite = file_ExitWrite;
    return 0;
}

/*****************************************************************************
 * dir_*: handler for the auxiliary directory format
 *****************************************************************************/
static char *psz_input_dir_name;
static size_t i_input_dir_len;
static uint64_t i_input_dir_file;
static uint64_t i_input_dir_delay;

static ssize_t dir_Read(void *p_buf, size_t i_len, FILE *p_input_aux, int i_input_fd)
{
    for (;;)
    {
        ssize_t i_ret = file_Read(p_buf, i_len, p_input_aux, i_input_fd);
        if (i_ret > 0)
            return i_ret;

        b_die = 0; /* we're not dead yet */
        close(i_input_fd);
        fclose(p_input_aux);
        i_input_fd = 0;
        p_input_aux = NULL;

        for (;;)
        {
            i_input_dir_file++;

            i_input_fd = OpenDirFile(psz_input_dir_name, i_input_dir_file,
                                     true, i_input_dir_len, &p_input_aux);
            if (i_input_fd > 0)
                break;

            if (i_input_dir_file * i_rotate_size + i_rotate_offset >
                i_first_stc + i_duration)
            {
                msg_Err(NULL, "end of files reached");
                thread_is_live = 0;//Added on 7aug
                b_die = 1;
                return 0;
            }

            msg_Warn(NULL, "missing segment");
        }
    }
}

static bool dir_Delay(void)
{
    uint64_t i_wall = pf_Date() - i_input_dir_delay;
    int64_t i_delay = i_stc - i_wall;

    if (i_delay > 0)
        pf_Sleep(i_delay);
    else if (i_delay < -MAX_LATENESS)
    {
        msg_Warn(NULL, "dropping late packet");
        return false;
    }
    return true;
}

static void dir_ExitRead(int i_input_fd)
{
    free(psz_input_dir_name);
    if (i_input_fd > 0)
    {
        close(i_input_fd);
    }
}

static int dir_InitRead(const char *psz_arg, size_t i_len,
                        off_t i_nb_skipped_chunks, int64_t i_pos, FILE *p_input_aux, int *i_input_fd)
{
    if (i_nb_skipped_chunks)
    {
        msg_Err(NULL, "unable to skip chunks with directory input");
        return -1;
    }

    if (i_pos <= 0)
        i_pos += real_Date();
    if (i_pos <= 0)
    {
        msg_Err(NULL, "invalid position");
        return -1;
    }
    i_first_stc = i_stc = i_pos;
    i_input_dir_delay = real_Date() - i_stc;

    psz_input_dir_name = strdup(psz_arg);
    i_input_dir_len = i_len;
    i_input_dir_file = GetDirFile(i_rotate_size, i_rotate_offset, i_pos);

    for (;;)
    {
        i_nb_skipped_chunks = LookupDirAuxFile(psz_input_dir_name,
                                               i_input_dir_file, i_stc,
                                               i_input_dir_len);
        if (i_nb_skipped_chunks >= 0)
            break;

        if (i_input_dir_file * i_rotate_size + i_rotate_offset >
            i_stc + i_duration)
        {
            msg_Err(NULL, "position not found");
            return -1;
        }

        i_input_dir_file++;
        msg_Warn(NULL, "missing segment");
    }

    *i_input_fd = OpenDirFile(psz_input_dir_name, i_input_dir_file,
                             true, i_input_dir_len, &p_input_aux);

    lseek(*i_input_fd, (off_t)i_len * i_nb_skipped_chunks, SEEK_SET);
    fseeko(p_input_aux, 8 * i_nb_skipped_chunks, SEEK_SET);

    pf_Date = real_Date;
    pf_Sleep = real_Sleep;
    pf_Read = dir_Read;
    pf_Delay = dir_Delay;
    pf_ExitRead = dir_ExitRead;
    return 0;
}

static char *psz_output_dir_name;
static size_t i_output_dir_len;
static uint64_t i_output_dir_file;

static ssize_t dir_Write(const void *p_buf, size_t i_len, int i_output_fd)
{
    uint64_t i_dir_file = GetDirFile(i_rotate_size, i_rotate_offset, i_stc);
    if (!i_output_fd || i_dir_file != i_output_dir_file)
    {
        if (i_output_fd)
        {
            close(i_output_fd);
            fclose(p_output_aux);
        }

        i_output_dir_file = i_dir_file;

        i_output_fd = OpenDirFile(psz_output_dir_name, i_output_dir_file,
                                   false, i_output_dir_len, &p_output_aux);
    }

    return file_Write(p_buf, i_len, i_output_fd);
}

static void dir_ExitWrite(int i_output_fd)
{
    free(psz_output_dir_name);
    if (i_output_fd)
    {
        close(i_output_fd);
        fclose(p_output_aux);
    }
}

static int dir_InitWrite(const char *psz_arg, size_t i_len, bool b_append, int *i_output_fd)
{
    psz_output_dir_name = strdup(psz_arg);
    i_output_dir_len = i_len;
    i_output_dir_file = 0;
    *i_output_fd = 0;

    pf_Date = real_Date;
    pf_Sleep = real_Sleep;
    pf_Write = dir_Write;
    pf_ExitWrite = dir_ExitWrite;

    return 0;
}

/*****************************************************************************
 * GetPCR: read PCRs to align RTP timestamps with PCR scale (RFC compliance)
 *****************************************************************************/
static void GetPCR(const uint8_t *p_buffer, size_t i_read_size)
{
    while (i_read_size >= TS_SIZE)
    {
        uint16_t i_pid = ts_get_pid(p_buffer);

        if (!ts_validate(p_buffer))
        {
            msg_Warn(NULL, "invalid TS packet (sync=0x%x)", p_buffer[0]);
        }
        else if ((i_pid == i_pcr_pid || i_pcr_pid == 8192) && ts_has_adaptation(p_buffer) && ts_get_adaptation(p_buffer) && tsaf_has_pcr(p_buffer))
        {
            i_pcr = tsaf_get_pcr(p_buffer) * 300 + tsaf_get_pcrext(p_buffer);
            i_pcr_stc = i_stc;
        }
        p_buffer += TS_SIZE;
        i_read_size -= TS_SIZE;
    }
}

/*****************************************************************************
 * FixCC: fix continuity counters
 *****************************************************************************/
static void FixCC(uint8_t *p_buffer, size_t i_read_size)
{
    while (i_read_size >= TS_SIZE)
    {
        uint16_t i_pid = ts_get_pid(p_buffer);

        if (!ts_validate(p_buffer))
        {
            msg_Warn(NULL, "invalid TS packet (sync=0x%x)", p_buffer[0]);
        }
        else
        {
            if (pi_pid_cc_table[i_pid] == 0x10)
            {
                msg_Dbg(NULL, "new pid entry %d", i_pid);
                pi_pid_cc_table[i_pid] = 0;
            }
            else if (ts_has_payload(p_buffer))
            {
                pi_pid_cc_table[i_pid] = (pi_pid_cc_table[i_pid] + 1) % 0x10;
            }
            ts_set_cc(p_buffer, pi_pid_cc_table[i_pid]);
        }
        p_buffer += TS_SIZE;
        i_read_size -= TS_SIZE;
    }
}

/*****************************************************************************
 * RestampPCR
 *****************************************************************************/
static void RestampPCR(uint8_t *p_ts)
{
    uint64_t i_pcr = tsaf_get_pcr(p_ts) * 300 + tsaf_get_pcrext(p_ts);
    bool b_discontinuity = tsaf_has_discontinuity(p_ts);

    if (i_last_pcr == TS_CLOCK_MAX)
        i_last_pcr = i_pcr;
    else
    {
        /* handle 2^33 wrap-arounds */
        uint64_t i_delta =
            (TS_CLOCK_MAX + i_pcr -
             (i_last_pcr % TS_CLOCK_MAX)) %
            TS_CLOCK_MAX;
        if (i_delta <= MAX_PCR_INTERVAL && !b_discontinuity)
            i_last_pcr = i_pcr;
        else
        {
            msg_Warn(NULL, "PCR discontinuity (%" PRIu64 ")", i_delta);
            i_last_pcr += i_stc - i_last_pcr_date;
            i_last_pcr %= TS_CLOCK_MAX;
            i_pcr_offset += TS_CLOCK_MAX + i_last_pcr - i_pcr;
            i_pcr_offset %= TS_CLOCK_MAX;
            i_last_pcr = i_pcr;
        }
    }
    i_last_pcr_date = i_stc;
    if (!i_pcr_offset)
        return;

    i_pcr += i_pcr_offset;
    i_pcr %= TS_CLOCK_MAX;
    tsaf_set_pcr(p_ts, i_pcr / 300);
    tsaf_set_pcrext(p_ts, i_pcr % 300);
    tsaf_clear_discontinuity(p_ts);
}

/*****************************************************************************
 * RestampTS
 *****************************************************************************/
static uint64_t RestampTS(uint64_t i_ts)
{
    i_ts += i_pcr_offset;
    i_ts %= TS_CLOCK_MAX;
    return i_ts;
}

/*****************************************************************************
 * Restamp: Restamp PCRs, DTSs and PTSs
 *****************************************************************************/
static void Restamp(uint8_t *p_buffer, size_t i_read_size)
{
    fprintf(stderr, "Restamp 2196\n");exit(0);
    while (i_read_size >= TS_SIZE)
    {
        if (!ts_validate(p_buffer))
        {
            msg_Warn(NULL, "invalid TS packet (sync=0x%x)", p_buffer[0]);
        }
        else
        {
            if (ts_has_adaptation(p_buffer) && ts_get_adaptation(p_buffer) &&
                tsaf_has_pcr(p_buffer))
                RestampPCR(p_buffer);

            uint16_t header_size = TS_HEADER_SIZE +
                                   (ts_has_adaptation(p_buffer) ? 1 : 0) +
                                   ts_get_adaptation(p_buffer);
            if (ts_get_unitstart(p_buffer) && ts_has_payload(p_buffer) &&
                header_size + PES_HEADER_SIZE_PTS <= TS_SIZE &&
                pes_validate(p_buffer + header_size) &&
                pes_get_streamid(p_buffer + header_size) !=
                    PES_STREAM_ID_PRIVATE_2 &&
                pes_validate_header(p_buffer + header_size) &&
                pes_has_pts(p_buffer + header_size)
                /* disable the check as this is a common mistake */
                /* && pes_validate_pts(p_buffer + header_size) */)
            {
                pes_set_pts(p_buffer + header_size,
                            RestampTS(pes_get_pts(p_buffer + header_size) * 300) /
                                300);

                if (header_size + PES_HEADER_SIZE_PTSDTS <= TS_SIZE &&
                    pes_has_dts(p_buffer + header_size)
                    /* && pes_validate_dts(p_buffer + header_size) */)
                    pes_set_dts(p_buffer + header_size,
                                RestampTS(pes_get_dts(p_buffer + header_size) * 300) /
                                    300);
            }
        }
        p_buffer += TS_SIZE;
        i_read_size -= TS_SIZE;
    }
}

/**************************************************************************
 *  code for shell script
 **************************************************************************/

unsigned int tokenize(const char *text, char delim, char ***output);
void reclaim2D(char ***store, unsigned int itemCount);

int hhmmss_to_secs(char *time)
{
    int ctr = 0;
    int seconds = 0;
    char *token = strtok(time, ":");
    while (token != NULL)
    {
        ctr++;
        if (ctr == 1)
        {
            seconds = seconds + atoi(token) * 3600;
        }
        else if (ctr == 2)
        {
            seconds = seconds + atoi(token) * 60;
        }
        else if (ctr == 3)
        {
            seconds = seconds + atoi(token);
        }
        else if (ctr == 4)
        {
            seconds = seconds + atoi(token) / 25;
        }

        token = strtok(NULL, ":");
    }
    return seconds;
}

void start_sub(long int SECONDS_S, char *ACT_DURATION, char *SUB_ID, long int *value)
{
    int64_t START;
    int64_t END;	
    char DURATION[MAX_WORD];
    int sec = SECONDS_S;
    long int tm, END_secs, OFFSET = 1;
    double ADVANCED = 0;
    float DELAYSET = 2.5;
    time(&tm);
    strcpy(DURATION, ACT_DURATION);
	//value = (long int *)malloc(2*sizeof(int));
	if (sec != 0)
	{
		ADVANCED = ADVANCED + SECONDS_S;
		START = 27000000 * ADVANCED + OFFSET;
		END_secs = hhmmss_to_secs(DURATION);
		END = 27000000 * (END_secs - DELAYSET); //Value of fixed delay subtracted from duration
		value[0] = START;
		value[1] = END;
		printf("value of start: %ld\n", value[0]);
		printf("value of end: %ld\n", value[1]);
	}
}

unsigned int tokenize(const char *text, char delim, char ***output)
{
    if ((*output) != NULL)
        return -1; /* I will allocate my own storage */
    int ndelims, i, j, ntokens, starttok, endtok;
    // First pass, count the number of delims
    i = 0;
    ndelims = 0;
    while (text[i] != '\0')
    {
        if (text[i] == delim)
            ndelims++;
        i++;
    }

    // The number of delims is one less than the number of tokens
    ntokens = ndelims + 1;

    // Now, allocate an array of (char*)'s equal to the number of tokens

    (*output) = (char **)malloc(sizeof(char *) * ntokens);

    // Now, loop through and extract each token
    starttok = 0;
    endtok = 0;
    i = 0;
    j = 0;
    while (text[i] != '\0')
    {
        // Reached the end of a token?
        if (text[i] == delim)
        {
            endtok = i;
            // Allocate a char array to hold the token
            (*output)[j] = (char *)malloc(sizeof(char) * (endtok - starttok + 1));
            // If the token is not empty, copy over the token
            if (endtok - starttok > 0)
                memcpy((*output)[j], &text[starttok], (endtok - starttok));
            // Null-terminate the string
            (*output)[j][(endtok - starttok)] = '\0';
            // The next token starts at i+1
            starttok = i + 1;
            j++;
        }
        i++;
    }

    // Deal with the last token
    endtok = i;
    // Allocate a char array to hold the token
    (*output)[j] = (char *)malloc(sizeof(char) * (endtok - starttok + 1));
    // If the token is not empty, copy over the token
    if (endtok - starttok > 0)
        memcpy((*output)[j], &text[starttok], (endtok - starttok));
    // Null-terminate the string
    (*output)[j][(endtok - starttok)] = '\0';
    return ntokens;
}

void reclaim2D(char ***store, unsigned int itemCount)
{
    int x;
    for (x = 0; itemCount < itemCount; ++x)
    {
        if ((*store)[x] != NULL)
            free((*store)[x]);
        (*store)[x] = NULL;
    }

    if ((*store) != NULL)
        free((*store));
    (*store) = NULL;
}
/***************************************************************************
 *	Automation Shell Script C code
 **************************************************************************/
pthread_t thread1[MAX_LANG], thread2[MAX_LANG],multicatThread[MAX_LANG];
pthread_t thread_tsudpsend[MAX_LANG];

void get_script_params(char **myarr, long int *SECONDS_S, char *ACT_DURATION, char *SUB_ID)
{

	long int FRAMES_S;
	char segment_number[MAXDATA];
	char segment_time[MAXDATA];
	long int segment_time_in_secs, SOM_in_secs = 36000, start_pos;
	char DURATION_REMAIN[MAXDATA];
	FRAMES_S = atoi(myarr[10]);
	*SECONDS_S = (FRAMES_S / 25);
	strcpy(SUB_ID, myarr[3]);
	strcpy(segment_number, myarr[6]);
	strcpy(segment_time, myarr[8]);						 //#segment start time in hh:mm:ss:ff
	segment_time_in_secs = hhmmss_to_secs(segment_time); //Segment start time in seconds
	start_pos = segment_time_in_secs - SOM_in_secs;
	strcpy(DURATION_REMAIN, myarr[9]);
	strncpy(ACT_DURATION, DURATION_REMAIN, 8);
	*SECONDS_S = *SECONDS_S + start_pos;
	//printf("SECONDS_S: %ld = %p\n", SECONDS_S, SECONDS_S);
}

void autoSwitch(int signum)
{
	fprintf(stderr, "Inside autoSwitch function, gUseBackupScommand : [%d]\n", gUseBackupScommand);
	if(gAutomationDataReceived == -1)
	{
		if(gUseBackupScommand == 0)
		{
			gUseBackupScommand = 1;
			fprintf(stderr, "Changed gUseBackupScommand : [%d]\n", gUseBackupScommand);
		}
		else
		{
			gUseBackupScommand = 0;
			fprintf(stderr, "Changed gUseBackupScommand : [%d]\n", gUseBackupScommand);
		}
	}
//	alarm(15); //commentd on 02052022
}

void *listenToggleScommand(void *arg)
{
	int sockfdToggleInput, returnToggleInput;
	char bufferToggleInput[MAXLINE];
	struct sockaddr_in serverAddressToggleInput;
    socklen_t lenToggleInput;
//	signal(SIGALRM, autoSwitch);
	if ((sockfdToggleInput = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
   		perror("socket creation failed");
    	exit(EXIT_FAILURE);
	}

	memset(&serverAddressToggleInput, 0, sizeof(serverAddressToggleInput));

	serverAddressToggleInput.sin_family = AF_INET;
	serverAddressToggleInput.sin_addr.s_addr = inet_addr(inputArgs->AutomationToggleCommandIP); //INADDR_ANY;
	serverAddressToggleInput.sin_port = htons(inputArgs->AutomationToggleCommandPort);

    if (bind(sockfdToggleInput, (struct sockaddr *) &serverAddressToggleInput, sizeof(serverAddressToggleInput)) < 0)
    {
        printf("sockfdToggleInput ERROR on binding\n");
        exit(0);
    }


  //  fprintf(stderr, "Listening for backup and main toggle input\n");
	while(1)
	{
//        fprintf(stderr, "Listening for backup and main toggle input\n");
		memset(bufferToggleInput, '\0', sizeof(bufferToggleInput));	
		returnToggleInput = recvfrom(sockfdToggleInput, (char *)bufferToggleInput, MAXLINE,
        	    	                    MSG_WAITALL, (struct sockaddr *)&serverAddressToggleInput,
            	    	                &lenToggleInput);

		bufferToggleInput[returnToggleInput] = '\0';
        fprintf(stderr, "Got toggle command : [%s]\n",bufferToggleInput);
		if(strstr(bufferToggleInput, "backupscript"))
		{
			gUseBackupScommand = 1;
		}
		if(strstr(bufferToggleInput, "mainscript"))
		{
    		gUseBackupScommand = 0;
		}
		/*else
		{
			gUseBackupScommand = -1;
		}*/
		
		if(strstr(bufferToggleInput,"mainreference"))
		{
			toggle_val = 0;
        	fprintf(stderr, "toggle_val : [%d]\n",toggle_val);
		}
		if(strstr(bufferToggleInput,"backupreference"))
		{
        	toggle_val = 1;
        	fprintf(stderr, "toggle_val : [%d]\n",toggle_val);
		}
	}
}

void *listenMainAutomationData(void *arg)
{
    int sockfd, n;
	char buffer[MAXLINE];
	struct sockaddr_in serverAddress, clientAddress;
	socklen_t len = sizeof(clientAddress);

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&serverAddress, 0, sizeof(serverAddress));
    memset(&clientAddress, 0, sizeof(clientAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = inet_addr(inputArgs->MainIPAutomation);
    serverAddress.sin_port = htons(inputArgs->MainPortAutomation);
	fprintf(stderr, "MainIPAutomation : [%s]\n", inputArgs->MainIPAutomation);
	fprintf(stderr, "inputArgs->MainPortAutomation : [%d]\n", inputArgs->MainPortAutomation);
    if (bind(sockfd, (const struct sockaddr *)&serverAddress,
             sizeof(serverAddress)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
	while (1)   
	{
        gUseBackupScommand = 0;
//		if(gUseBackupScommand == 0)
//			alarm(9);
		n = recvfrom(sockfd, (char *)buffer, MAXLINE,
           	         MSG_WAITALL, (struct sockaddr *)&clientAddress,
               	     &len);
		if(gUseBackupScommand == 0)
		{
//		alarm(0);
			memset(gAutomationbuffer, '\0', sizeof(gAutomationbuffer));
			strcpy(gAutomationbuffer, buffer);
			fprintf(stderr, "Automation command : [%s]\n", gAutomationbuffer);
		}
	}
	fprintf(stderr, "Data received main thread : %d\n", n);
}
void *listenBackupAutomationData(void *arg)
{
	int sockfdBackup, n;
	char buffer[MAXLINE];
	struct sockaddr_in serverAddressBackup, clientAddressBackup;
	socklen_t len = sizeof(clientAddressBackup);
    sockfdBackup = -1;

	/*Socket creation for backup*/
//	if(!strcmp(inputArgsAllLanguages[0].UseAutomationBackup, "YES"))

    fprintf(stderr, "Creating automation backup socket\n");
    if ((sockfdBackup = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("Backup socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&serverAddressBackup, 0, sizeof(serverAddressBackup));
    memset(&clientAddressBackup, 0, sizeof(clientAddressBackup));

    serverAddressBackup.sin_family = AF_INET;
    serverAddressBackup.sin_addr.s_addr = inet_addr(inputArgs->BackupIPAutomation);
    serverAddressBackup.sin_port = htons(inputArgs->BackupPortAutomation);

    if (bind(sockfdBackup, (const struct sockaddr *)&serverAddressBackup,
                sizeof(serverAddressBackup)) < 0)
    {
        perror("Backup server bind failed");
        exit(EXIT_FAILURE);
    }

    while(0)
//	while (1)   //commented on 02052022
	{
		if(gUseBackupScommand == 1)
			alarm(9);
		n = recvfrom(sockfdBackup, (char *)buffer, MAXLINE,
           	         MSG_WAITALL, (struct sockaddr *)&clientAddressBackup,
               	     &len);
		if(gUseBackupScommand == 1)
		{
		    alarm(0);
			memset(gAutomationbuffer, '\0', sizeof(gAutomationbuffer));
    		strcpy(gAutomationbuffer, buffer);
			fprintf(stderr, "Receiving from backup : [%s]\n", gAutomationbuffer);
		}
	}
	fprintf(stderr, "Data received backup thread : %d\n", n);
}

void *script(void *arg)
{
    fprintf(stderr,"%s:start_func\n", __func__);
	int stop_while = 0;	
    int  n, c, i = 0;
    char udp_in[MAXLINE];
    char *myarr[MAXLINE] = {0};
    char SUB_ID[MAXDATA];
    char ACT_DURATION[MAXDATA];
    char P_TYPE[MAXDATA];
    char **tokens = NULL;
    int DELAYSET = 2.5;
    int64_t SECONDS_S;
    int64_t value[2];
    char COMMAND[MAXDATA];
    char *S_COMMAND[MAX_LANG];
    char prog_ID[100];
    /***********************************************/
    int sockfd, ret_recvfrm;
	char buffer[MAXLINE];
	struct sockaddr_in serverAddress, clientAddress;
	socklen_t len = sizeof(clientAddress);

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&serverAddress, 0, sizeof(serverAddress));
    memset(&clientAddress, 0, sizeof(clientAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = inet_addr(inputArgs->MainIPAutomation);
    serverAddress.sin_port = htons(inputArgs->MainPortAutomation);
	fprintf(stderr, "MainIPAutomation : [%s]\n", inputArgs->MainIPAutomation);
	fprintf(stderr, "inputArgs->MainPortAutomation : [%d]\n", inputArgs->MainPortAutomation);
    if (bind(sockfd, (const struct sockaddr *)&serverAddress,
             sizeof(serverAddress)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
	while (1)   
	{
		ret_recvfrm = recvfrom(sockfd, (char *)buffer, MAXLINE,
           	         MSG_WAITALL, (struct sockaddr *)&clientAddress,
               	     &len);
			memset(gAutomationbuffer, '\0', sizeof(gAutomationbuffer));
			strcpy(gAutomationbuffer, buffer);
			fprintf(stderr, "Automation command : [%s]\n", gAutomationbuffer);

    /***********************************************/
        if(gAutomationbuffer[0] != '\0')
        {
            memset(udp_in, '\0', sizeof(udp_in));
            strcpy(udp_in, gAutomationbuffer);
            c = tokenize(udp_in, ',', &tokens);
            for (i = 0; i < c; i++)
            {
                myarr[i] = tokens[i];
            }

            reclaim2D(&tokens, c);

            strcpy(COMMAND, myarr[0]);
            strcpy(P_TYPE, myarr[12]);
            memset(prog_ID, '\0', sizeof(prog_ID));
            strcpy(prog_ID, myarr[3]);
            //fprintf(stderr, "prog_ID : [%s]\n", prog_ID);
            //if (!strcmp(P_TYPE, "p") && (b_die == 0))
            //if ( (!strcmp(P_TYPE, "p") && (b_die == 0)) && ( (!strcmp(COMMAND, "START")) || (!strcmp(COMMAND, "PLAY")))) //Commented on 6Aug
            if ( (!strcmp(P_TYPE, "p")) && ( (!strcmp(COMMAND, "START")) || (!strcmp(COMMAND, "PLAY"))))
            {

                get_script_params(myarr, &SECONDS_S, ACT_DURATION, SUB_ID);

                if (!strcmp(COMMAND, "START"))
                    printf("Start received\n");
            
                else if ((!strcmp(COMMAND, "PLAY")) && stop_while == 0)
                {
                    start_sub(SECONDS_S, ACT_DURATION, SUB_ID, &value[0]);
                    stop_while++;
                    DEBUG_PRINTF("gTotalLanguage : %d\n\n\n\n", gTotalLanguage);
                    for (i = 0; i < gTotalLanguage; i++)
                    {
                        int id = i+1;
                        int *pointerId;
                        char file_exists_check[100];
                        pointerId = (int *)malloc(sizeof(int));
                        *pointerId = i+1;
                        S_COMMAND[i] = (char *)malloc(S_COMMAND_LENGTH);
                        
                      /*  sprintf(S_COMMAND[i], 
                                    "./multicat -p %d -k %ld -d %ld %sRWD-PG-004880_%s.ts -U 224.1.1.2:811%d ", 
                                    inputArgsAllLanguages[i].SubtitlePID, 
                                    value[0], value[1], 
                                    inputArgsAllLanguages[i].Subtitle, 
                                    inputArgsAllLanguages[i].Language, id);*/
                        sprintf(S_COMMAND[i], 
                                    "./multicat -p %d -k %ld -d %ld %s%s_%s.ts -U 224.1.1.2:811%d ", 
                                    inputArgsAllLanguages[i].SubtitlePID, 
                                    value[0], value[1], 
                                    inputArgsAllLanguages[i].Subtitle, 
                                    prog_ID,
                                    inputArgsAllLanguages[i].Language, id);
								fprintf(stderr,"Lang in thread create : [%s], SCOMMAND : [%s] \n", inputArgsAllLanguages[i].Language, S_COMMAND[i]);
/*Anil                        fprintf(stderr, 
                                    "./multicat -p %d -k %ld -d %ld %sRWD-PG-003770_%s.ts -U 127.0.0.1:821%d ", 
                                    inputArgsAllLanguages[i].SubtitlePID, 
                                    value[0], value[1], 
                                    inputArgsAllLanguages[i].Subtitle, 
                                    inputArgsAllLanguages[i].Language, id);*/
                        thread_is_live = 1;
                        b_die = 0;
                        /*Resetting global variable*/
                        #if 0
                        b_die = 0;
                        i_ttl = 0;
                        b_sleep = true;
                        i_pcr_pid = 0;
                        b_overwrite_ssrc = false;
                        i_ssrc = 0;
                        b_input_udp = false;
                        b_output_udp = false;
                        i_duration = 0;
                        b_raw_packets = false;
                        pi_pid_cc_table = NULL;

                        /* PCR/PTS/DTS restamping */
                        i_last_pcr_date = 0;
                        i_pcr_offset = 0;

                        b_error = 0;
                        i_rtp_seqnum = 0;
                        i_stc = 0; /* system time clock, used for date calculations */
                        i_first_stc = 0;
                        i_pcr = 0; 
                        i_pcr_stc = 0; /* for RTP/TS output */
                        #endif
                        //create_thread[id - 1] = -1;
                        memset(file_exists_check, '\0', sizeof(file_exists_check));
                        sprintf(file_exists_check,"%s%s_%s.ts",inputArgsAllLanguages[i].Subtitle, prog_ID, inputArgsAllLanguages[i].Language);
                        if (access(file_exists_check, F_OK) == 0)
                        {
                            fprintf(stderr, "File:[%s] exists,Creating threads for Id : %d\n",file_exists_check, id);
                            pthread_create(&multicatThread[id], NULL, multicat, (void *)S_COMMAND[i]);
                            /*if(create_thread[id - 1] == -1)
                                 sleep(0.5);
                                fprintf(stderr, "Thread value : %d\n\n", create_thread[id - 1]);
                            */
                            /*if(create_thread[id] == 1)
                            */
                            pthread_create(&thread1[id], NULL, tsReference, (void *)pointerId);
                            pthread_create(&thread2[id], NULL, tsReplace, (void *)pointerId);
                            pthread_create(&thread_tsudpsend[id], NULL, tsudpsend, (void *)pointerId);
                        }
                        else //if(create_thread[id] == 0)
                        {
                            fprintf(stderr, "Threads are not created for Id : %d\n", id);
                        }
                        //sleep(1);
                    }
                }
            }
            else if (!strcmp(P_TYPE, "f") || !strcmp(COMMAND, "END") || !strcmp(COMMAND, "STOP") || !strcmp(COMMAND, "SKIP") || b_die == 1)
            {
                //sleep(DELAYSET - 1);
                DEBUG_PRINTF("\n*FOUND STOP CONDITION*\n");
                fprintf(stderr, "gAutomationbuffer in script : [%s]\n", gAutomationbuffer);
                if (stop_while >= 1)
                {
                    fprintf(stderr, "Stopping threads\n");
                    thread_is_live = 0;
                    stop_while = 0;
                    sleep(DELAYSET - 1);
                    //b_die = 0;//Commented on 6aug
                    /*sleep(1);
                    for (i = 1; i <= gTotalLanguage; i++)
                    {
                        pthread_cancel(multicatThread[i]);
                        pthread_cancel(thread1[i]);
                        pthread_cancel(thread2[i]);
                        pthread_cancel(thread_tsudpsend[i]);
                    }*/
                }
            }
        }
    }
}

/*Function to extract start and end value from S_COMMAND string and return in numeric form*/
void extractStartEnd(char *sCommand, int i_argc, int64_t *start, uint64_t *end)
{
    int loop_ctr, copy_ctr, fill_ctr = 0;
    char sStart[20], sEnd[20];
    memset(sStart, '\0', sizeof(sStart));
    memset(sEnd, '\0', sizeof(sEnd));
    for (loop_ctr = 0; loop_ctr < 122; loop_ctr++)
    {

        if ((sCommand[loop_ctr] == '-') && (sCommand[loop_ctr + 1] == 'k'))
        {
            for(copy_ctr = loop_ctr+3; ; copy_ctr++)
            {
                if(sCommand[copy_ctr] == ' ')
                    break;
                sStart[fill_ctr] = sCommand[copy_ctr];
                fill_ctr++;
            }
        }
        else if ((sCommand[loop_ctr] == '-') && (sCommand[loop_ctr + 1] == 'd'))
        {
            fill_ctr = 0;
            for(copy_ctr = loop_ctr+3; ; copy_ctr++)
            {
                if(sCommand[copy_ctr] == ' ')
                    break;
                sEnd[fill_ctr] = sCommand[copy_ctr];
                fill_ctr++;
            }
        }
    }
    *start = strtoull(sStart, NULL, 0);
    *end = strtoull(sEnd, NULL, 0);
}

/*****************************************************************************
 *	Multicat Code
 *****************************************************************************/
void *multicat(void *argv)
{
    DEBUG_PRINTF("\n%s:start_func\n", __func__);
    int i_argc;
    char **pp_argv;
    int i_output_fd; //Local variable created instead of global variable
    FILE *p_input_aux = NULL;
    int i_input_fd = 0;
    int i_priority = -1;
    const char *psz_syslog_tag = NULL;
    bool b_passthrough = false;
    bool b_restamp = false;
    int i_stc_fd = -1;
    off_t i_skip_chunks = 0, i_nb_chunks = -1;
    int64_t i_seek = 0;
    bool b_append = false;
    uint8_t *p_buffer, *p_read_buffer;
    size_t i_max_read_size, i_max_write_size;
    struct sigaction sa;
    int ii = 0, ctr = 0, row = 0, col = 0;
    sigset_t set;
    char word[MAX_WORD];
    i_argc = 0;
    int portIterator = 0;
    char S_COMMAND[COMMAND_LENGTH];
    char langname[COMMAND_LENGTH];
    char mylangname[COMMAND_LENGTH];
    memset(S_COMMAND, '\0', COMMAND_LENGTH);
    strcpy(S_COMMAND, argv);

    portIterator = S_COMMAND[strlen(S_COMMAND) - 2] - '0';

    /* if (gfile_check > gTotalLanguage)
       {
       exit(0);
       }*/
    for (ctr = 0; ctr < strlen(S_COMMAND); ctr++)
    {
        if (S_COMMAND[ctr] == ' ')
            i_argc++;
    }
    pp_argv = malloc((i_argc) * sizeof(char *));
    ii = 0;
    for (ctr = 0; ctr < strlen(S_COMMAND); ctr++)
    {
        if (S_COMMAND[ctr] == ' ')
        {
            fprintf(stderr, "word : [%s], strlen : %d\n", word, strlen(word));
            //pp_argv[row] = malloc(strlen(word) + 1);
            pp_argv[row] = malloc(strlen(word) );
            //memset(pp_argv[row], '\0', sizeof(&pp_argv[row]));
            memset(pp_argv[row], '\0', strlen(word));
            memcpy(pp_argv[row], word, strlen(word));
            if(row == 7)
            {
                memset(langname, '\0', sizeof(langname));
                memset(mylangname, '\0', sizeof(mylangname));
                memcpy(mylangname, word, strlen(word));
            }
            row++;
            col = 0;
            ii = 0;
            memset(word, '\0', sizeof(word));
        }
        else
        {
            memcpy(&word[ii], &S_COMMAND[ctr], 1);
            ii++;
        }
        col++;
    }
   /* memset(langname, '\0', sizeof(langname));
    memset(mylangname, '\0', sizeof(mylangname));
    memcpy(mylangname, pp_argv[7], strlen(pp_argv[7]));*/
            fprintf(stderr,"mylangname : [%s], strlen : %d\n", mylangname, strlen(pp_argv[7]));
    //DEBUG_PRINTF("S_COMMAND : [");
    for (ii = 0; ii < i_argc; ii++)
    {
     //   DEBUG_PRINTF("%s ", pp_argv[ii]);
        if (ii == 7)
        {
            strcpy(langname, pp_argv[ii]);
        }
    }

    extractStartEnd(S_COMMAND, i_argc, &i_seek, &i_duration);
    i_pcr_pid = inputArgsAllLanguages[0].SubtitlePID;
    b_output_udp = true;

    if (psz_syslog_tag != NULL)
        msg_Openlog(psz_syslog_tag, LOG_NDELAY, LOG_USER);

    /* Open sockets */
    //if (udp_InitRead(langname, i_asked_payload_size, i_skip_chunks,
    if (udp_InitRead(mylangname, i_asked_payload_size, i_skip_chunks,
                i_seek, &i_input_fd) < 0)
    {
        int i_ret;
        //mode_t i_mode = StatFile(langname);
        mode_t i_mode = StatFile(mylangname);
        if (!i_mode)
        {
        fprintf(stderr, "Line : %d, imode : [%d]\n", __LINE__, i_mode);
            msg_Err(NULL, "Subtitle input File is not found , Please provide the correct file");
            fprintf(stderr,"File : [%s]\n", langname);
            fprintf(stderr,"File : [%s]\n", mylangname);
            b_die = 1;

            create_thread[portIterator - 1] = 0;
            return NULL;
            //exit(EXIT_FAILURE);
        }
        create_thread[portIterator - 1] = 1;
        //fprintf(stderr,"Line:%d,Func:%s,1langname : [%s]\n", __LINE__,__func__,langname);
        fprintf(stderr,"Line:%d,Func:%s,1mylangname : [%s]\n", __LINE__,__func__,mylangname);

       /* if (S_ISDIR(i_mode))
            i_ret = dir_InitRead(langname, i_asked_payload_size, 
                    i_skip_chunks, i_seek, p_input_aux, &i_input_fd);
        else if (S_ISCHR(i_mode) || S_ISFIFO(i_mode))
            i_ret = stream_InitRead(langname, i_asked_payload_size,
                    i_skip_chunks, i_seek, &i_input_fd);
        else*/
        i_ret = file_InitRead(mylangname, i_asked_payload_size,
                i_skip_chunks, i_seek, &p_input_aux, &i_input_fd);

        if (i_ret == -1)
        {
            msg_Err(NULL, "couldn't open input, exiting");
            exit(EXIT_FAILURE);
        }
        b_input_udp = true; /* We don't need no, RTP header */
    }
    memset(langname, '\0', sizeof(langname));
    sprintf(langname, "127.0.0.1:821%d", portIterator);
    if (udp_InitWrite(langname, i_asked_payload_size, b_append, &i_output_fd) < 0)
    {
        int i_ret;
        mode_t i_mode = StatFile(langname);

        fprintf(stderr, "Line : %d, imode : [%d]\n", __LINE__, i_mode);
        if (S_ISDIR(i_mode))
            i_ret = dir_InitWrite(langname, i_asked_payload_size,
                    b_append, &i_output_fd);
        else if (S_ISCHR(i_mode) || S_ISFIFO(i_mode))
            i_ret = stream_InitWrite(langname, i_asked_payload_size,
                    b_append, &i_output_fd);
        else
        {
            fprintf("langname in else : [%s]\n", langname);
            i_ret = file_InitWrite(langname, i_asked_payload_size,
                    b_append, &i_output_fd);
        }
        if (i_ret == -1)
        {
            msg_Err(NULL, "couldn't open output, exiting");
            exit(EXIT_FAILURE);
        }
        b_output_udp = true; /* We don't need no, RTP header */
    }

    srand(time(NULL) * gettid());
    i_max_read_size = i_asked_payload_size + (b_input_udp ? 0 : i_rtp_header_size);
    i_max_write_size = i_asked_payload_size + (b_output_udp ? 0 : (b_input_udp ? RTP_HEADER_SIZE : i_rtp_header_size));
    p_buffer = malloc((i_max_read_size > i_max_write_size) ? i_max_read_size : i_max_write_size);
    p_read_buffer = p_buffer + ((b_input_udp && !b_output_udp) ? RTP_HEADER_SIZE : 0);
    if (b_input_udp && !b_output_udp)
        i_rtp_seqnum = rand() & 0xffff;


    /* Real-time priority */
    if (i_priority > 0)
    {
        DEBUG_PRINTF("Setting thread priority\t");
        struct sched_param param;
        int i_error;

        memset(&param, 0, sizeof(struct sched_param));
        param.sched_priority = i_priority;
        if ((i_error = pthread_setschedparam(pthread_self(), SCHED_RR,
                        &param)))
        {
            msg_Warn(NULL, "couldn't set thread priority: %s",
                    strerror(i_error));
        }
    }

    /* Set signal handlers */
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = SigHandler;
    sigfillset(&set);

    if (sigaction(SIGTERM, &sa, NULL) == -1 ||
            sigaction(SIGHUP, &sa, NULL) == -1 ||
            sigaction(SIGINT, &sa, NULL) == -1 ||
            sigaction(SIGPIPE, &sa, NULL) == -1)
    {
        msg_Err(NULL, "couldn't set signal handler: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if(p_input_aux == NULL)
    {
        DEBUG_PRINTF("\np_input_aux is not initialized [Error:1001] \nExiting!!!\n");
    }
    /* Main loop */
    //while (!b_die)
    fprintf(stderr,"b_die:%d, thread_is_live:%d\n",b_die, thread_is_live);
    while (!b_die && thread_is_live)
    {
        ssize_t i_read_size = pf_Read(p_read_buffer, i_max_read_size, p_input_aux, i_input_fd);
        uint8_t *p_payload;
        size_t i_payload_size;
        uint8_t *p_write_buffer;
        size_t i_write_size;

        if (i_duration && i_stc > i_first_stc + i_duration)
            break;

        if (i_read_size <= 0)
            continue;

        if (b_sleep && pf_Delay != NULL)
            if (!pf_Delay())
                goto dropped_packet;

        /* Determine start and size of payload */
        if (!b_input_udp)
        {
            if (!rtp_check_hdr(p_read_buffer))
                msg_Warn(NULL, "invalid RTP packet received");
                fprintf(stderr,"S_COMMAND : [%s]\n", argv);
            p_payload = rtp_payload(p_read_buffer);
            i_payload_size = p_read_buffer + i_read_size - p_payload;
        }
        else
        {
            p_payload = p_read_buffer;
            i_payload_size = i_read_size;
        }

        /* Skip last incomplete TS packet */
        i_read_size -= i_payload_size % TS_SIZE;
        i_payload_size -= i_payload_size % TS_SIZE;

        /* Pad to get the asked payload size */
        while (i_payload_size + TS_SIZE <= i_asked_payload_size)
        {
            ts_pad(&p_payload[i_payload_size]);
            i_read_size += TS_SIZE;
            i_payload_size += TS_SIZE;
        }

        /* Fix continuity counters */
        if (pi_pid_cc_table != NULL)
            FixCC(p_payload, i_payload_size);

        /* Restamp */
        if (b_restamp)
            Restamp(p_payload, i_payload_size);

        /* Prepare header and size of output */
        if (b_output_udp)
        {
            p_write_buffer = p_payload;
            i_write_size = i_payload_size;
        }
        else /* RTP output */
        {
            if (b_input_udp)
            {
                p_write_buffer = p_buffer;
                i_write_size = i_payload_size + RTP_HEADER_SIZE;

                rtp_set_hdr(p_write_buffer);
                rtp_set_type(p_write_buffer, RTP_TYPE_TS);
                rtp_set_seqnum(p_write_buffer, i_rtp_seqnum);
                i_rtp_seqnum++;

                if (i_pcr_pid)
                {
                    GetPCR(p_payload, i_payload_size);
                    rtp_set_timestamp(p_write_buffer,
                            (i_pcr + (i_stc - i_pcr_stc)) / 300);
                }
                else
                {
                    /* This isn't RFC-compliant but no one really cares */
                    rtp_set_timestamp(p_write_buffer, i_stc / 300);
                }
                rtp_set_ssrc(p_write_buffer, (uint8_t *)&i_ssrc);
            }
            else /* RTP out put, RTP input */
            {
                p_write_buffer = p_read_buffer;
                i_write_size = i_read_size;

                if (i_pcr_pid)
                {
                    if (rtp_get_type(p_write_buffer) != RTP_TYPE_TS)
                        msg_Warn(NULL, "input isn't MPEG transport stream");
                    else
                        GetPCR(p_payload, i_payload_size);
                    rtp_set_timestamp(p_write_buffer,
                            (i_pcr + (i_stc - i_pcr_stc)) / 300);
                }
                if (b_overwrite_ssrc)
                    rtp_set_ssrc(p_write_buffer, (uint8_t *)&i_ssrc);
            }

        }


        pf_Write(p_write_buffer, i_write_size, i_output_fd);
        if (b_passthrough)
            if (write(STDOUT_FILENO, p_write_buffer, i_write_size) != i_write_size)
                msg_Warn(NULL, "write(stdout) error (%s)", strerror(errno));

dropped_packet:
        if (i_stc_fd != -1)
        {
            char psz_stc[256];
            size_t i_len = sprintf(psz_stc, "<?xml version=\"1.0\" encoding=\"utf-8\"?><MULTICAT><STC value=\"%" PRIu64 "\"/></MULTICAT>", i_stc);
            memset(psz_stc + i_len, '\n', sizeof(psz_stc) - i_len);
            if (lseek(i_stc_fd, 0, SEEK_SET) == (off_t)-1)
                msg_Warn(NULL, "lseek date file failed (%s)",
                        strerror(errno));
            if (write(i_stc_fd, psz_stc, sizeof(psz_stc)) != sizeof(psz_stc))
                msg_Warn(NULL, "write date file error (%s)", strerror(errno));
        }

        if (i_nb_chunks > 0)
            i_nb_chunks--;
        if (!i_nb_chunks)
            break;
    }

    free(pi_pid_cc_table);
    free(p_buffer);

    pf_ExitRead(i_input_fd);
    pf_ExitWrite(i_output_fd);

    if (psz_syslog_tag != NULL)
        msg_Closelog();

    //return b_error ? EXIT_FAILURE : EXIT_SUCCESS;
    int ret = b_error ? EXIT_FAILURE : EXIT_SUCCESS;
    int *ret_ptr = &ret;
    return ret_ptr;	
}

/********************************************************************
 *                       Validate  config file fields				*
 ********************************************************************/
int validateConfigFileFields(char *str,char *field)
{
    if(strlen(str) > 0)
    {
        return 1;
    }
    else
    {
        fprintf(stderr,"Please provide correct data in config file's field : %s\n",field);
        exit(EXIT_FAILURE);
    }
}


/*******************************************************************
 *			Read config file
 ******************************************************************/
int read_conf(const char *pConfigFile, struct scriptArguments *inputArgs, char *lang1[MAX_LANG], char *allPIDs[MAX_LANG])
{
    char *ptr, *ptr1;
    FILE *fp;
    unsigned int position = 0;
    int linenum = 0;
    fp = fopen(pConfigFile, "r");
    char line_buff[256];

    if (!fp)
    {
        printf("Error in opening config file\n");
        return 0;
    }
    else
    {
        while (fgets(line_buff, 256, fp) != NULL)
        {
            /**increment line*/
            linenum++;

            if (line_buff[0] == '#')
                continue;

            if ((ptr = strstr(line_buff, "=")) && ((ptr1 = strstr(line_buff, "\n")) || (ptr1 = strstr(line_buff, "\r"))))
            {
                position = (int)(ptr - line_buff);
                *ptr1 = '\0';
            }
            if (strstr(line_buff, "LANGUAGE"))
            {
                char allLang[MAX_WORD];
                memset(allLang, '\0', sizeof(allLang));
                strcpy(allLang, line_buff + position + 1);
                int i = 0, j = 0;
                char *p = strtok(allLang, ",");
                while (p != NULL)
                {
                    lang1[i] = (char *)malloc(4);
                    //lang1[i++] = p;
                    strcpy(lang1[i], p);
                    i++;
                    p = strtok(NULL, ",");
                    j++;
                }
                gTotalLanguage = j;
                for (i = 0; i < gTotalLanguage; i++)
                    fprintf(stderr, "language: %s\n", lang1[i]);
                memset(line_buff, 0, sizeof(line_buff));
            }
            if (strstr(line_buff, "REPLACE_PID"))
            {
                char pid[MAX_WORD];
                memset(pid, '\0', sizeof(pid));
                strcpy(pid, line_buff + position + 1);
                int i = 0, j = 0;
                char *p = strtok(pid, ",");
                while (p != NULL)
                {
                    allPIDs[i] = (char *)malloc(4);
                    //lang1[i++] = p;
                    strcpy(allPIDs[i], p);
                    i++;
                    p = strtok(NULL, ",");
                    j++;
                }
                for (i = 0; i < j; i++)
                    fprintf(stderr, "PIDs : %s\n", allPIDs[i]);
                memset(line_buff, 0, sizeof(line_buff));
            }
            else if(strstr(line_buff,"SUBTITLE"))
            {
                strcpy(inputArgs->Subtitle,line_buff+position+1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Subtitle files path = [%s]\n",inputArgs->Subtitle);
            }
            else if(strstr(line_buff,"INPUT_FILE_PID"))
            {
                char sSubtitlePID[PID_LENGTH];
                memset(sSubtitlePID, '\0', sizeof(sSubtitlePID));
                strcpy(sSubtitlePID,line_buff+position+1);
                inputArgs->SubtitlePID = atoi(sSubtitlePID);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Subtitle PID : [%d]\n", inputArgs->SubtitlePID);
                if(inputArgs->SubtitlePID == 0)
                {
                    fprintf(stderr,"Please provide correct data in INPUT_FILE_PID\n");
                }
            }
            else if(strstr(line_buff,"REFERENCE_IP"))
            {
                strcpy(inputArgs->ReferenceIp, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Reference IP : [%s]\n", inputArgs->ReferenceIp);
            }
            else if(strstr(line_buff,"REFERENCE_PORT"))
            {
                char sReferencePort[PORT_LENGTH];
                memset(sReferencePort, '\0', sizeof(sReferencePort));
                strcpy(sReferencePort, line_buff + position + 1);
                inputArgs->ReferencePort = atoi(sReferencePort);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Reference Port : [%d]\n", inputArgs->ReferencePort);
                if(inputArgs->ReferencePort == 0)
                {
                    fprintf(stderr,"Please provide correct data in REFERENCE_PORT\n");
                }
            }
            else if(strstr(line_buff,"REFERENCE_BACKUP_IP"))
            {
                strcpy(inputArgs->BackupReferenceIp, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Backup Reference IP : [%s]\n", inputArgs->BackupReferenceIp);
            }
            else if(strstr(line_buff,"REFERENCE_BACKUP_PORT"))
            {
                char sReferencePort[PORT_LENGTH];
                memset(sReferencePort, '\0', sizeof(sReferencePort));
                strcpy(sReferencePort, line_buff + position + 1);
                inputArgs->BackupReferencePort = atoi(sReferencePort);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Backup Reference Port : [%d]\n", inputArgs->BackupReferencePort);
                if(inputArgs->BackupReferencePort == 0)
                {
                    fprintf(stderr,"Please provide correct data in BACKUP_REFERENCE_PORT\n");
                }
            }
            else if(strstr(line_buff,"REFERENCE_PID"))
            {
                char sReferencePID[PID_LENGTH];
                memset(sReferencePID, '\0', sizeof(sReferencePID));
                strcpy(sReferencePID, line_buff + position + 1);
                inputArgs->ReferencePID = atoi(sReferencePID);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Reference PID : [%d]\n", inputArgs->ReferencePID);
                if(inputArgs->ReferencePID == 0)
                {
                    fprintf(stderr,"Please provide correct data in REFERENCE_PID\n");
                }
            }
            else if(strstr(line_buff,"OFFSET"))
            {
                char sOffset[PID_LENGTH];
                memset(sOffset, '\0', sizeof(sOffset));
                strcpy(sOffset, line_buff + position + 1);
                inputArgs->Offset = atoi(sOffset);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Reference Offset : [%d]\n", inputArgs->Offset);
                if(inputArgs->Offset == 0)
                {
                    fprintf(stderr,"Please provide correct data in sOffset\n");
                }
            }
            else if(strstr(line_buff,"USE_AUTOMATION_BACKUP"))
            {
                strcpy(inputArgs->UseAutomationBackup, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Use Automation backup : [%s]\n", inputArgs->UseAutomationBackup);
            }
            else if(strstr(line_buff,"AUTOMATION_TOGGLE_COMMAND_IP"))
            {
                strcpy(inputArgs->AutomationToggleCommandIP, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation toggle command IP : [%s]\n", inputArgs->AutomationToggleCommandIP);
            }
            else if(strstr(line_buff,"AUTOMATION_TOGGLE_COMMAND_PORT"))
            {
                char sAutomationToggleCommandPort[PID_LENGTH];
                memset(sAutomationToggleCommandPort, '\0', sizeof(sAutomationToggleCommandPort));
                strcpy(sAutomationToggleCommandPort, line_buff + position + 1);
                inputArgs->AutomationToggleCommandPort = atoi(sAutomationToggleCommandPort);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation toggle command port : [%d]\n", inputArgs->AutomationToggleCommandPort);
            }
            else if(strstr(line_buff,"BACKUP_IP_AUTOMATION"))
            {
                strcpy(inputArgs->BackupIPAutomation, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation backup IP : [%s]\n", inputArgs->BackupIPAutomation);
            }
            else if(strstr(line_buff,"BACKUP_PORT_AUTOMATION"))
            {
                char sBackupPortAutomation[PID_LENGTH];
                memset(sBackupPortAutomation, '\0', sizeof(sBackupPortAutomation));
                strcpy(sBackupPortAutomation, line_buff + position + 1);
                inputArgs->BackupPortAutomation = atoi(sBackupPortAutomation);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation backup port : [%d]\n", inputArgs->BackupPortAutomation);
            }
            else if(strstr(line_buff,"MAIN_IP_AUTOMATION"))
            {
                strcpy(inputArgs->MainIPAutomation, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation main IP : [%s]\n", inputArgs->MainIPAutomation);
            }
            else if(strstr(line_buff,"MAIN_PORT_AUTOMATION"))
            {
                char sMainPortAutomation[PID_LENGTH];
                memset(sMainPortAutomation, '\0', sizeof(sMainPortAutomation));
                strcpy(sMainPortAutomation, line_buff + position + 1);
                inputArgs->MainPortAutomation = atoi(sMainPortAutomation);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Automation main port : [%d]\n", inputArgs->MainPortAutomation);
            }
            else if(strstr(line_buff,"DEBUG_PRINT"))
            {
                strcpy(debug_print, line_buff + position + 1);
                if(validateConfigFileFields(debug_print,"DEBUG_PRINT"))
                {
                    memset(line_buff,0,sizeof(line_buff));
                    fprintf(stderr,"DEBUG_PRINT : [%s]\n", debug_print);
                }
            }
            else if(strstr(line_buff,"ZABBIX_USERNAME"))
            {
                strcpy(gZABBIX_USERNAME,line_buff+position+1);
                printf("Zabbix_user: %s\n",gZABBIX_USERNAME);
                memset(line_buff,0,sizeof(line_buff));
            }
            else if(strstr(line_buff,"ZABBIX_PASSWORD"))
            {
                strcpy(gZABBIX_PASSWORD,line_buff+position+1);
                printf("zabbix_paswd: %s\n",gZABBIX_PASSWORD);
                memset(line_buff,0,sizeof(line_buff));
            }
            else if(strstr(line_buff,"ZABBIX_IP"))
            {
                strcpy(gZABBIX_IP,line_buff+position+1);
                printf("zabbix_ip: %s\n",gZABBIX_IP);
                memset(line_buff,0,sizeof(line_buff));
            }
            else if(strstr(line_buff,"CHANNEL_NAME"))
            {
                strcpy(gCHANNEL_NAME,line_buff+position+1);
                printf("Channel_name: %s\n",gCHANNEL_NAME);
                memset(line_buff,0,sizeof(line_buff));
            }
            else if(strstr(line_buff,"HOST_NAME"))
            {
                strcpy(gHOST_NAME,line_buff+position+1);
                printf("HOST_NAME:%s\n",gHOST_NAME);
                memset(line_buff,0,sizeof(line_buff));
            }
            else if(strstr(line_buff,"IP_OUTPUT_TO_MUX"))
            {
                strcpy(inputArgs->IPOutputToMux, line_buff + position + 1);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"IP Output to MUX : [%s]\n", inputArgs->IPOutputToMux);
            }
            else if(strstr(line_buff,"PORT_OUTPUT_TO_MUX"))
            {
                char sPortOutputToMux[PID_LENGTH];
                memset(sPortOutputToMux, '\0', sizeof(sPortOutputToMux));
                strcpy(sPortOutputToMux, line_buff + position + 1);
                inputArgs->PortOutputToMux = atoi(sPortOutputToMux);
                memset(line_buff,0,sizeof(line_buff));
                fprintf(stderr,"Port Output to MUX : [%d]\n", inputArgs->PortOutputToMux);
            }

        }
    }
    fclose(fp);
    return 1;
}

void help()
{
    printf("\nUsage: <binary> <config file> -d(optional)\n");
    printf("-d will enable debug print to be capture in file\n");
    printf("If multiple languages provided then their respective subtitle PIDs should be entered in REPLACE_PID field\n");
    printf("#Config file contents :\n");
    printf("###############################\n");
    printf("#UDP IP of live stream\n");
    printf("REFERENCE_IP =225.1.1.2\n");
    printf("#UDP port of live stream\n");
    printf("REFERENCE_PORT =5112\n");
    printf("#Video PID of live stream\n");
    printf("REFERENCE_PID =201,101\n");
    printf("#PID of subtitle to be restamp\n");
    printf("REPLACE_PID =108\n");
    printf("#Video PID of language input\n");
    printf("INPUT_FILE_PID =1001\n");
    printf("#Reference Offset\n");
    printf("OFFSET =1\n");
    printf("#Subtitle langauges separated by comma\n");
    printf("LANGUAGE =sim,idn\n");
    printf("#Path of subtitle langauge ts files\n");
    printf("SUBTITLE =/sub_data/rewind/TS/\n");
    printf("###############################\n\n");
}
char *allLanguagesArray[MAX_LANG];
char *allPIDsArray[MAX_LANG];
int main(int i_argc, char **pp_argv)
{
    pthread_t scriptThread, toggelScommandThread, mainAutomationThread, backupAutomationThread, tsRefAutoSwitchThread;
    pthread_t tsRefMainInputThread, tsRefBackupInputThread;
    int r_value, instance_ctr, i;
    //    struct scriptArguments *inputArgs;
    //   char *allLanguagesArray[MAX_LANG];
    //	signal(SIGALRM, autoSwitch); commented on 02052022
    //alarm(10);
    gAutomationDataReceived = -1;
    memset(gAutomationbuffer, '\0', sizeof(gAutomationbuffer));
    for(i = 0; i < i_argc; i++)
    {
        if(!(strcmp(pp_argv[i], "-d")) || !(strcmp(pp_argv[i], "-D"))){
            freopen("debugPrints.txt", "w", stderr);
            fprintf(stderr,"Printing in debug file\n\n");
            printf("Printing in debug file\n\n");
        }
    }

    inputArgs = (struct scriptArguments *)malloc(sizeof(struct scriptArguments));

    if(i_argc < 2)
    {
        help();
        exit(0);
    }

    r_value = read_conf(pp_argv[1], inputArgs, &allLanguagesArray[0], &allPIDsArray[0]);

    for(instance_ctr = 0; instance_ctr < gTotalLanguage; instance_ctr++)
    {
        fprintf(stderr, "In main allLanguagesArray : [%s]\n", allLanguagesArray[instance_ctr]);
        strcpy(inputArgsAllLanguages[instance_ctr].Language, allLanguagesArray[instance_ctr]);   
        strcpy(inputArgsAllLanguages[instance_ctr].Subtitle, inputArgs->Subtitle);
        strcpy(inputArgsAllLanguages[instance_ctr].ReferenceIp, inputArgs->ReferenceIp);
        strcpy(inputArgsAllLanguages[instance_ctr].BackupReferenceIp, inputArgs->BackupReferenceIp);
        strcpy(inputArgsAllLanguages[instance_ctr].UseAutomationBackup, inputArgs->UseAutomationBackup);
        strcpy(inputArgsAllLanguages[instance_ctr].AutomationToggleCommandIP, inputArgs->AutomationToggleCommandIP);
        inputArgsAllLanguages[instance_ctr].ReferencePort = inputArgs->ReferencePort;
        inputArgsAllLanguages[instance_ctr].BackupReferencePort = inputArgs->BackupReferencePort;
        inputArgsAllLanguages[instance_ctr].ReferencePID = inputArgs->ReferencePID;
        inputArgsAllLanguages[instance_ctr].ReplacePID = atoi(allPIDsArray[instance_ctr]);
        inputArgsAllLanguages[instance_ctr].SubtitlePID = inputArgs->SubtitlePID;
        inputArgsAllLanguages[instance_ctr].Offset = inputArgs->Offset;
    }
    if(!strcasecmp(debug_print,"ON"))
    {
        debug_var = 1; 
    }
    else if(!strcasecmp(debug_print, "OFF"))
    {
        debug_var = 0;
    }
    else
    {
        debug_var = 2;
    }	

    fprintf(stderr, "In Main Backup reference IP : [%s]\n",inputArgs->BackupReferenceIp);	
    fprintf(stderr, "In Main Backup reference PORT : [%d]\n",inputArgs->BackupReferencePort);	

    fprintf(stderr, "In Main Backup reference IP : [%s]\n",inputArgsAllLanguages[0].BackupReferenceIp);	
    fprintf(stderr, "In Main Backup reference PORT : [%d]\n",inputArgsAllLanguages[0].BackupReferencePort);	
    if (r_value)
    {
        /*		pthread_create(&tsRefMainInputThread, NULL, tsRefMainInput, NULL);
                pthread_create(&tsRefBackupInputThread, NULL, tsRefBackupInput, NULL);*/
        //		sleep(1);
        //        pthread_create(&mainAutomationThread, NULL, listenMainAutomationData, NULL);
        //      pthread_create(&backupAutomationThread, NULL, listenBackupAutomationData, NULL);
        pthread_create(&toggelScommandThread, NULL, listenToggleScommand, NULL);
        pthread_create(&tsRefAutoSwitchThread, NULL, tsRefAutoSwitch, NULL);
        pthread_create(&scriptThread, NULL, script, NULL);
        pthread_join(scriptThread, 0);
        pthread_join(mainAutomationThread, 0);
        pthread_join(backupAutomationThread, 0);
        pthread_join(toggelScommandThread, 0);
    }
    return 0;
}
