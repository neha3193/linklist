/**
 * @file
 * HW-Accelerated decoding example with filtering video.
 *
 * @example hw_decode.c
 * This example shows how to do HW-accelerated decoding with output
 * frames from the HW video surfaces and detect black frame, freeze frame & silence detect .
 */

#define _XOPEN_SOURCE 600 /* for usleep */
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <pthread.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "crc32.h"
#include "klbitstream_readwriter.h"
#include "header.h"
#include "rheader.h"

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

//#define PORT   5050

char *video_filter_descr;
const char *audio_filter_descr;
char *Multicast_SRC;

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_video_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *video_filter_graph;
static int video_stream_index = -1;
static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;

extern LOG_INFO Log_info;

int gVideoPid, gAudioPid;

/*Audio code*/
static AVCodecContext *dec_audio_ctx;
AVFilterContext *buffersink_audio_ctx;
AVFilterContext *buffersrc_audio_ctx;
AVFilterGraph *filter_audio_graph;
static int audio_stream_index = -1;

static int init_video_filters(const char *filters_descr)
{
	char args[512];
	int ret = 0;
	const AVFilter *buffersrc = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

	video_filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !video_filter_graph)
	{
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* buffer video source: the decoded frames from the decoder will be inserted here. */
	/*snprintf(args, sizeof(args),
	  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
	  dec_video_ctx->width, dec_video_ctx->height, dec_video_ctx->pix_fmt,
	  time_base.num, time_base.den,
	  dec_video_ctx->sample_aspect_ratio.num, dec_video_ctx->sample_aspect_ratio.den);
	  */
	snprintf(args, sizeof(args),
			"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			dec_video_ctx->width, dec_video_ctx->height, 23,
			time_base.num, time_base.den,
			dec_video_ctx->sample_aspect_ratio.num, dec_video_ctx->sample_aspect_ratio.den);

	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
			args, NULL, video_filter_graph);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto end;
	}
	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
			NULL, NULL, video_filter_graph);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
		goto end;
	}
	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
			AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
		goto end;
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	if ((ret = avfilter_graph_parse_ptr(video_filter_graph, filters_descr,
					&inputs, &outputs, NULL)) < 0)
		goto end;
	if ((ret = avfilter_graph_config(video_filter_graph, NULL)) < 0)
		goto end;
end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}

static int init_audio_filters(const char *filters_descr)
{
	char args[512];
	int ret = 0;
	const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	static const enum AVSampleFormat out_sample_fmts[] = {AV_SAMPLE_FMT_S16, -1};
	static const int64_t out_channel_layouts[] = {AV_CH_LAYOUT_MONO, -1};
	static const int out_sample_rates[] = {8000, -1};
	const AVFilterLink *outlink;
	AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
	filter_audio_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_audio_graph)
	{
		ret = AVERROR(ENOMEM);
		goto end;
	}
	/* buffer audio source: the decoded frames from the decoder will be inserted here. */
	if (!dec_audio_ctx->channel_layout)
		dec_audio_ctx->channel_layout = av_get_default_channel_layout(dec_audio_ctx->channels);
	snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
			time_base.num, time_base.den, dec_audio_ctx->sample_rate,
			av_get_sample_fmt_name(dec_audio_ctx->sample_fmt), dec_audio_ctx->channel_layout);
	ret = avfilter_graph_create_filter(&buffersrc_audio_ctx, abuffersrc, "in",
			args, NULL, filter_audio_graph);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
		goto end;
	}
	/* buffer audio sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(&buffersink_audio_ctx, abuffersink, "out",
			NULL, NULL, filter_audio_graph);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
		goto end;
	}
	ret = av_opt_set_int_list(buffersink_audio_ctx, "sample_fmts", out_sample_fmts, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
		goto end;
	}
	ret = av_opt_set_int_list(buffersink_audio_ctx, "channel_layouts", out_channel_layouts, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
		goto end;
	}
	ret = av_opt_set_int_list(buffersink_audio_ctx, "sample_rates", out_sample_rates, -1,
			AV_OPT_SEARCH_CHILDREN);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
		goto end;
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_audio_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_audio_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	if ((ret = avfilter_graph_parse_ptr(filter_audio_graph, filters_descr,
					&inputs, &outputs, NULL)) < 0)
		goto end;
	if ((ret = avfilter_graph_config(filter_audio_graph, NULL)) < 0)
		goto end;
	/* Print summary of the sink buffer
	 * Note: args buffer is reused to store channel layout string */
	outlink = buffersink_audio_ctx->inputs[0];
	av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
	av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
			(int)outlink->sample_rate,
			(char *)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
			args);
end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}

static int hw_dec_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
	int err = 0;

	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
					NULL, NULL, 0)) < 0)
	{
		fprintf(stderr, "Failed to create specified HW device.\n");
		return err;
	}
	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

	return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
		const enum AVPixelFormat *pix_fmts)
{
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++)
	{
		if (*p == hw_pix_fmt)
			return *p;
	}

	fprintf(stderr, "Failed to get HW surface format.\n");
	return AV_PIX_FMT_NONE;
}

int gBlackAlarmFlag, gSilenceAlarmFlag, gFreezeAlarmFlag;
void *sendAlarm(void *args)
{
//  int blackAlarmFlag, silenceAlarmFlag, freezeAlarmFlag;
	int blackCtr = 0;
	int silenceCtr = 0;
	int freezeCtr = 0;
	char blackPrintMessege[100];
	char freezePrintMessege[100];
	char silencePrintMessege[100];
	char pidPrintMessege[100];
	int msec = 0;
	int msec1 = 0;
	int msec2 = 0;
	int sockfd;
	double blackRatioPercentage = 0.0;
	double silenceLevelValue = 0.0;
	clock_t before, before1, before2;
	struct sockaddr_in   servaddr;
    gBlackAlarmFlag = -1;
    gSilenceAlarmFlag = -1;
    gFreezeAlarmFlag = -1;
    fprintf(stderr, "Inside sendAlarm\n");

    /*Socket creation to send filters messeges on UDP socket*/
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 )
    {
		perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
   	memset(&servaddr, 0, sizeof(servaddr));

    /* Filling server information */
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(1234);
    servaddr.sin_addr.s_addr = INADDR_ANY;


    while(1)
    {
        if(gBlackAlarmFlag == 1)
        {	
			blackCtr++;
			if(blackCtr == 1)
			{
				before = clock();
			}
			clock_t difference = clock() - before;
			msec = difference * 1000 / CLOCKS_PER_SEC;
			blackRatioPercentage = atof(Log_info.BlackLevel);
			//fprintf(stderr, "Black ratio : %.2f\n\n\n\n",blackRatioPercentage*100);
            //fprintf(stderr, "Black Level\t%s\t%d\tAlarm\n", Log_info.BlackLevel,msec/1000);

			memset(blackPrintMessege, '\0', sizeof(blackPrintMessege));
            //sprintf(blackPrintMessege, "Black Level\t%-08s\t%d\tAlarm\n", Log_info.BlackLevel,msec/1000);
            sprintf(blackPrintMessege, "Black Level    %.2f   %-08d Alarm\n", blackRatioPercentage*100, msec/1000);
			sendto(sockfd, (const char *)blackPrintMessege, strlen(blackPrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(0.10);
        }
        else
        {
			blackRatioPercentage = atof(Log_info.BlackLevel);
			blackCtr = 0;
			memset(blackPrintMessege, '\0', sizeof(blackPrintMessege));
            //sprintf(blackPrintMessege, "Black Level\t%-08s\t%d\tCleared\n", Log_info.BlackLevel,msec/1000);
            sprintf(blackPrintMessege, "Black Level    %.2f   %-08d OK\n", blackRatioPercentage*100, msec/1000);
			sendto(sockfd, (const char *)blackPrintMessege, strlen(blackPrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(1);
//            fprintf(stderr, "Black Level\t%s\t%d\tCleared\n", Log_info.BlackLevel,msec/1000);
        }
        if(gSilenceAlarmFlag == 1)
        {	silenceCtr++;
			if(silenceCtr == 1)
			{
				before1 = clock();
			}
			clock_t difference = clock() - before1;
			msec1 = difference * 1000 / CLOCKS_PER_SEC;
			silenceLevelValue = atof(Log_info.SilenceLevel);
            //fprintf(stderr, "Silence Level\t%d\tAlarm\n", msec1/1000);
			memset(silencePrintMessege, '\0', sizeof(silencePrintMessege));
            //sprintf(silencePrintMessege, "Silence Level %.2f   %-08d Alarm\n", silenceLevelValue, msec1/1000);
            sprintf(silencePrintMessege, "Silence Level %-08s %-08d Alarm\n", " ", msec1/1000);
            sendto(sockfd, (const char *)silencePrintMessege, strlen(silencePrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(0.10);

        }
        else
        {
			silenceCtr = 0;
            //fprintf(stderr, "Silence Level\t%d\tCleared\n", msec1/1000);
			silenceLevelValue = atof(Log_info.SilenceLevel);
			memset(silencePrintMessege, '\0', sizeof(silencePrintMessege));
            //sprintf(silencePrintMessege, "Silence Level %.2f   %-08d OK\n", silenceLevelValue, msec1/1000);
            sprintf(silencePrintMessege, "Silence Level %-08s %-08d OK\n", " ", msec1/1000);
            sendto(sockfd, (const char *)silencePrintMessege, strlen(silencePrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(1);
        }
        if(gFreezeAlarmFlag == 1)
        {	freezeCtr++;
			if(freezeCtr == 1)
			{
				before2 = clock();
			}
			clock_t difference = clock() - before2;
			msec2 = difference * 1000 / CLOCKS_PER_SEC;
            //fprintf(stderr, "Freeze Level\t%d\tAlarm\n",msec2/1000);
			memset(freezePrintMessege, '\0', sizeof(freezePrintMessege));
            sprintf(freezePrintMessege, "Freeze Level  %-08s %-08d Alarm\n"," ",msec2/1000);
            sendto(sockfd, (const char *)freezePrintMessege, strlen(freezePrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(0.10);
        }
        else
        {
			freezeCtr = 0;
            //fprintf(stderr, "Freeze Level\t%d\tCleared\n", msec2/1000);
			memset(freezePrintMessege, '\0', sizeof(freezePrintMessege));
            sprintf(freezePrintMessege, "Freeze Level  %-08s %-08d OK\n", " ", msec2/1000);
            sendto(sockfd, (const char *)freezePrintMessege, strlen(freezePrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(1);
        }
        if(Log_info.Bitrate == -1)
        {	
			memset(pidPrintMessege, '\0', sizeof(pidPrintMessege));
            sprintf(pidPrintMessege, "PID Missing  1\n\n");
            sendto(sockfd, (const char *)pidPrintMessege, strlen(pidPrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(0.10);
        }
        else
        {
			memset(pidPrintMessege, '\0', sizeof(pidPrintMessege));
            sprintf(pidPrintMessege, "PID Missing  0\n\n");
            sendto(sockfd, (const char *)pidPrintMessege, strlen(pidPrintMessege),
                                   MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                   sizeof(servaddr));
			sleep(1);
        }
/*
        if(gSilenceAlarmFlag == 1)
        {
            fprintf(stderr, "Got silence frames\n");
        }
        if(gFreezeAlarmFlag == 1)
        {
            fprintf(stderr, "Got freeze frames\n");
        }
  
      sleep(1);
*/
    }
    return (void *)0;
}



int hardware( )
{
	int ret, stream_ctr = 0;
	int videopid, audiopid;
	int sockfd;
	int blackStartIndicatorFlag, freezeStartIndicatorFlag, silenceStartIndicatorFlag;
	/*Code for demo review*/
	int milliSec = 0, alarmTrigger = 5;
	clock_t blackStartTimer, freezeStartTimer, silenceStartTimer;// = clock();
	blackStartTimer = freezeStartTimer = silenceStartTimer = -1;
	/*Code for demo review end*/
	char printBlackMessages[100], blackBuffer[100];
	char printFreezeMessages[100], freezeBuffer[100];
	char printSilenceMessages[100], silenceBuffer[100];
    struct sockaddr_in   servaddr;
	AVDictionaryEntry *blackTag, *blackTag1, *freezeTag, *freezeTag1, *silenceTag, *silenceTag1, *tag_ratio, *tag_RMS;
	AVStream *video = NULL;
	AVCodec *dec_video = NULL;
	AVCodec *dec_audio;
	AVPacket *packet = NULL;
	enum AVHWDeviceType type;
	int i;
	AVFrame *frame_audio = av_frame_alloc();
	AVFrame *filt_audio_frame = av_frame_alloc();
	AVFrame *frame_video = av_frame_alloc();
	AVFrame *filt_video_frame = av_frame_alloc();
	AVFrame *sw_frame = av_frame_alloc();
	AVFrame *tmp_frame = NULL;
    packet = av_packet_alloc();
    if (!frame_video || !filt_video_frame || !packet)
    {
        perror("Could not allocate frame or packet!!");
        exit(1);
    }
	/*Socket creation to send filters messeges on UDP socket*/
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 )
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    memset(&servaddr, 0, sizeof(servaddr));

    /* Filling server information */
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(atoi(gUDP_PORT));
    servaddr.sin_addr.s_addr = INADDR_ANY;
    /*Turn off the ffmpeg logs*/
//    av_log_set_level(AV_LOG_QUIET);
    //av_log_set_level(AV_LOG_DEBUG);
    av_log_set_level(AV_LOG_VERBOSE);
    //av_log_set_level(AV_LOG_ERROR);
    /*Populate these variables with correct values of video and audio pid*/
    gVideoPid = hwvideo_pid;
    gAudioPid = hwaud_pid[0];

    video_filter_descr = (char *)malloc(100 * sizeof(char));
    if(!(strcasecmp(gBlackDetect, "ON")) && !(strcasecmp(gFreezeDetect, "ON")))
    {
        sprintf(video_filter_descr, 
                "select='not(mod(n,5)'),blackdetect=d=%s:pix_th=%s,freezedetect=n=-%sdB:d=%s",
                gblackDuration, gblackThreshold,
                gfreezeNoise, gfreezeDuration);
        //	printf("video_filter_descr: %s \n",video_filter_descr);
    }
    else if(!(strcasecmp(gBlackDetect, "ON")))
    {
        sprintf(video_filter_descr, "select='not(mod(n,5)'),blackdetect=d=%s:pix_th=%s", gblackDuration, gblackThreshold);
    }
    else if(!(strcasecmp(gFreezeDetect, "ON")))
    {
        sprintf(video_filter_descr, "select='not(mod(n,5)'),freezedetect=n=-%sdB:d=%s", gfreezeNoise, gfreezeDuration);
    }
    else 
    {
        free(video_filter_descr);
        video_filter_descr = NULL;
    }

    //strcpy(video_filter_descr,"blackdetect=d=1.5:pix_th=0.10:pic_th=0.70");
    if(!(strcasecmp(gSilenceDetect, "ON")))
    {
        audio_filter_descr = (char *)malloc(100 * sizeof(char));
        sprintf(audio_filter_descr, "silencedetect=n=-%sdB:d=%s", gsilenceNoise, gsilenceDuration);
        //sprintf(filter_audio_descr, "silencedetect=n=-%sdB:d=%s,astats=metadata=1:reset=1", gsilenceNoise, gsilenceDuration);
    }

    type = av_hwdevice_find_type_by_name("vaapi");
    if (type == AV_HWDEVICE_TYPE_NONE)
    {
        fprintf(stderr, "Device type vaapi is not supported.\n");
        fprintf(stderr, "Available device types:");
        while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    /* open the input file */
    Multicast_SRC = (char *)malloc(100 * sizeof(char));
    //sprintf(Multicast_SRC, "udp://%s:%s", gMULTICAST_IP,gMULTICAST_PORT); //Commented on 01042022 Anil
    sprintf(Multicast_SRC, "udp://%s:%s?fifo_size=5000000&reuse=1&overrun_nonfatal=1", gMULTICAST_IP,gMULTICAST_PORT); 
    if (avformat_open_input(&fmt_ctx, Multicast_SRC, NULL, NULL) != 0)
    {
        fprintf(stderr, "Cannot open input file \n");
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
    {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /*Searching for audio mapped pid*/
    for(stream_ctr = 0; stream_ctr < fmt_ctx->nb_streams; stream_ctr++)
    {
        if(gAudioPid == fmt_ctx->streams[stream_ctr]->id)
            break;
    }
    audiopid = stream_ctr;

    /* select the audio stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, audiopid, -1, &dec_audio, 0);
    // ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec_audio, 0);
    if (ret < 0)
    {
       av_log(NULL, AV_LOG_ERROR, "Cannot find a audio stream in the input file\n");
        return ret;
    }
    audio_stream_index = ret;  

    /* create audio decoding context */
    dec_audio_ctx = avcodec_alloc_context3(dec_audio);
    if (!dec_audio_ctx)
        return AVERROR(ENOMEM);

    avcodec_parameters_to_context(dec_audio_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);

    /* init the audio decoder */
    if ((ret = avcodec_open2(dec_audio_ctx, dec_audio, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        return ret;
    }

    /*Searching for video mapped pid*/
    for(stream_ctr = 0; stream_ctr < fmt_ctx->nb_streams; stream_ctr++)
    {
        if(gVideoPid == fmt_ctx->streams[stream_ctr]->id)
            break;
    }
    videopid = stream_ctr; 

    /* find the video stream information */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, videopid, -1, &dec_video, 0);
    //    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0)
    {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream_index = ret;
    //printf("video_stream_index : %d\n", video_stream_index);
    for (i = 0;; i++)
    {
        const AVCodecHWConfig *config = avcodec_get_hw_config(dec_video, i);
        if (!config)
        {
            fprintf(stderr, "Decoder %s does not support device type %s.\n",
                    dec_video->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == type)
        {
            hw_pix_fmt = config->pix_fmt;
            break;
        }
    }

    if (!(dec_video_ctx = avcodec_alloc_context3(dec_video)))
        return AVERROR(ENOMEM);


    video = fmt_ctx->streams[video_stream_index];
    // if (avcodec_parameters_to_context(dec_video_ctx, video->codecpar) < 0)
    avcodec_parameters_to_context(dec_video_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
    //     return -1;

    dec_video_ctx->get_format = get_hw_format;

    if (hw_dec_init(dec_video_ctx, type) < 0)
        return -1;

    if ((ret = avcodec_open2(dec_video_ctx, dec_video, NULL)) < 0)
    {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream_index);
        return -1;
    }

    if(!(strcasecmp(gBlackDetect, "ON")) || (!(strcasecmp(gFreezeDetect, "ON")) ))
    {
        if(video_filter_descr != NULL)
        {
            if ((ret = init_video_filters(video_filter_descr)) < 0)
                goto end;
        }
    }
    if(!(strcasecmp(gSilenceDetect, "ON")))
    {
        if(audio_filter_descr != NULL)
        {
            if ((ret = init_audio_filters(audio_filter_descr)) < 0)
                goto end;
        }
    }   
    /* open the file to dump raw data */
    strcpy(Log_info.Freeze_Frame, "NO");
    strcpy(Log_info.SilenceLevel, "NO");
    FILE * fptr_errorLog;
//    fptr_errorLog = fopen("daemonErrorLog.txt","a");
    while (1)
    {
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
        {
            fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
            //break;
            fptr_errorLog = fopen("daemonErrorLog.txt","a");
            fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
            fclose(fptr_errorLog);
            continue;
        }
        if((!(strcasecmp(gBlackDetect, "ON")) || (!(strcasecmp(gFreezeDetect, "ON")) )))
        {
            if(video_filter_descr != NULL)
            {
                if (packet->stream_index == video_stream_index)
                {
                    ret = avcodec_send_packet(dec_video_ctx, packet);
                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the dec\n");
                        fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                        //break;
                        fptr_errorLog = fopen("daemonErrorLog.txt","a");
                        fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                        fclose(fptr_errorLog);
                        continue;
                    }
                    while (ret >= 0)
                    {
                        ret = avcodec_receive_frame(dec_video_ctx, frame_video);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            //fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            break;
                        } else if (ret < 0) {
                            fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                            fprintf(stderr, "Error while receiving a frame from the decoder\n");
                            //goto end;
                            break;
                        }
                        if (frame_video->format == hw_pix_fmt)
                        {
                            /* retrieve data from GPU to CPU */
                            if ((ret = av_hwframe_transfer_data(sw_frame, frame_video, 0)) < 0)
                            {
                                fprintf(stderr, "Error transferring the data to system memory\n");
                                fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                //goto end;
                                break;
                            }
                            tmp_frame = sw_frame;
                        }
                        else
                        {
                            tmp_frame = frame_video;
                        }


                        tmp_frame->pts = frame_video->best_effort_timestamp;
                        /* push the decoded frame into the filtergraph */
                        ret = av_buffersrc_add_frame_flags(buffersrc_ctx, tmp_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
                        if (ret < 0)
                        {
                            av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                            fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            break;
                        }

                        /* pull filtered frames from the filtergraph */
                        while (1)
                        {
                            ret = av_buffersink_get_frame(buffersink_ctx, filt_video_frame);
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            {
                                //fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                break;
                            }
                            if (ret < 0)
                            {
                                fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                fptr_errorLog = fopen("daemonErrorLog.txt","a");
                                fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                fclose(fptr_errorLog);
                                break;
                                //goto end;
                            }
                            /*Code for demo review*/
                            if ((tag_ratio = av_dict_get(filt_video_frame->metadata,"lavfi.picture_black_ratio", tag_ratio, AV_DICT_IGNORE_SUFFIX)))
                            {   
                                //fprintf(stderr,"\n%s  = %s\t", tag_ratio->key, tag_ratio->value);
                                memset(Log_info.BlackLevel, '\0', sizeof(Log_info.BlackLevel));
                                strcpy(Log_info.BlackLevel, tag_ratio->value);
                            }
                            /*Code end for demo review*/
                            //
                            /*Capture black and freeze detect prints code start*/
                            blackTag = NULL;
                            blackTag = av_dict_get(filt_video_frame->metadata, "lavfi.black_start", blackTag, AV_DICT_IGNORE_SUFFIX);
                            if ((blackTag != NULL) && (blackStartIndicatorFlag == 0))
                            {
                                blackStartIndicatorFlag = 1;
                                memset(printBlackMessages, '\0', sizeof(printBlackMessages));
                                sprintf(printBlackMessages,"%s = %s", blackTag->key, blackTag->value);
                                blackStartTimer = clock();
                            }
                            if(blackStartIndicatorFlag == 1)
                            {
                                clock_t difference = clock() - blackStartTimer;
                                milliSec = difference * 1000 / CLOCKS_PER_SEC;

                                if(milliSec >= 1) /*Here 1ms is trigger value to match*/
                                {
                                    gBlackAlarmFlag = 1;
                                }
                            }

                            blackTag1 = NULL;
                            blackTag1 = av_dict_get(filt_video_frame->metadata, "lavfi.black_end", blackTag1, AV_DICT_IGNORE_SUFFIX);
                            if((blackTag1 != NULL) && (blackStartIndicatorFlag == 1))
                            {
                                /*fprintf(stderr,"\n[%s]\n", printBlackMessages);
                                  fprintf(stderr,"[%s = %s]\n", blackTag1->key, blackTag1->value); 
                                  */
                                gBlackAlarmFlag = 0;
                                blackStartIndicatorFlag = 0;
                                sprintf(blackBuffer,"%s\t%s = %s\n",printBlackMessages, blackTag1->key, blackTag1->value);
                                //fprintf(stderr,"Sending black buffer : [%s]\n", blackBuffer);
                                /*sendto(sockfd, (const char *)blackBuffer, strlen(blackBuffer),
                                  MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                  sizeof(servaddr));*/
                            }
                            freezeTag = NULL;
                            freezeTag = av_dict_get(filt_video_frame->metadata, "lavfi.freezedetect.freeze_start", freezeTag, AV_DICT_IGNORE_SUFFIX);
                            if ((freezeTag != NULL) && (freezeStartIndicatorFlag == 0))
                            {
                                memset(Log_info.Freeze_Frame, '\0', sizeof(Log_info.Freeze_Frame));
                                strcpy(Log_info.Freeze_Frame, "YES");

                                freezeStartIndicatorFlag = 1;
                                memset(printFreezeMessages, '\0', sizeof(printFreezeMessages));
                                sprintf(printFreezeMessages,"%s = %s", freezeTag->key, freezeTag->value);
                            }
                            if(freezeStartIndicatorFlag == 1)
                            {
                                clock_t difference = clock() - blackStartTimer;
                                milliSec = difference * 1000 / CLOCKS_PER_SEC;

                                if(milliSec >= 1)
                                {
                                    gFreezeAlarmFlag = 1;
                                }
                            }

                            freezeTag1 = NULL;
                            freezeTag1 = av_dict_get(filt_video_frame->metadata, "lavfi.freezedetect.freeze_end", freezeTag1, AV_DICT_IGNORE_SUFFIX);
                            if((freezeTag1 != NULL) && (freezeStartIndicatorFlag == 1))
                            {
                                memset(Log_info.Freeze_Frame, '\0', sizeof(Log_info.Freeze_Frame));
                                strcpy(Log_info.Freeze_Frame, "NO");

                                /*fprintf(stderr,"\n[%s]\n", printFreezeMessages);
                                  fprintf(stderr,"[%s = %s]\n", freezeTag1->key, freezeTag1->value);*/
                                freezeStartIndicatorFlag = 0;
                                gFreezeAlarmFlag = 0;
                                sprintf(freezeBuffer,"%s\t%s = %s\n",printFreezeMessages, freezeTag1->key, freezeTag1->value);
                                //fprintf(stderr,"Sending freeze buffer : [%s]\n", freezeBuffer);
                                /*sendto(sockfd, (const char *)freezeBuffer, strlen(freezeBuffer),
                                  MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                  sizeof(servaddr));*/
                            }
                            /*Capture black and freeze detect prints code end*/
                            av_frame_unref(filt_video_frame);
                        }
                        av_frame_unref(tmp_frame);
                        av_frame_unref(frame_video);
                        av_frame_unref(sw_frame);
                    }
                }
            }
        }
        if(audio_filter_descr != NULL)
        {
            /*Audio begins*/
            if(!(strcasecmp(gSilenceDetect, "ON")))
            {
                if (packet->stream_index == audio_stream_index)
                {
                    ret = avcodec_send_packet(dec_audio_ctx, packet);
                    if (ret < 0)
                    {
                        fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                        av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                        //break;
                        fptr_errorLog = fopen("daemonErrorLog.txt","a");
                        fprintf(fptr_errorLog,"Error while sending a packet to the decoder\n");
                        fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                        fclose(fptr_errorLog);
                        continue;
                    }
                    while (ret >= 0)
                    {
                        ret = avcodec_receive_frame(dec_audio_ctx, frame_audio);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        {
                            fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            break;
                        }
                        else if (ret < 0)
                        {
                            fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            fptr_errorLog = fopen("daemonErrorLog.txt","a");
                            fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                            av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                            fclose(fptr_errorLog);
                            break;
                            //goto end;
                        }
                        if (ret >= 0)
                        {
                            /* push the audio data from decoded frame into the filtergraph */
                            if (av_buffersrc_add_frame_flags(buffersrc_audio_ctx, frame_audio, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
                            {
                                fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                fptr_errorLog = fopen("daemonErrorLog.txt","a");
                                fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
                                fclose(fptr_errorLog);
                                break;
                            }
                            /* pull filtered audio from the filtergraph */
                            while (1)
                            {
                                ret = av_buffersink_get_frame(buffersink_audio_ctx, filt_audio_frame);
                                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                                {
                                    //fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                    break;
                                }
                                if (ret < 0)
                                {
                                    fprintf(stderr,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                    fptr_errorLog = fopen("daemonErrorLog.txt","a");
                                    fprintf(fptr_errorLog,"Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                    printf("Error at : %d line no, Function : %s\n", __LINE__, __func__);
                                    fclose(fptr_errorLog);
                                    break;
                                    //goto end;
                                }
                                /*	if ((tag_RMS = av_dict_get(filt_audio_frame->metadata, "lavfi.astats.1.RMS_level", tag_RMS, AV_DICT_IGNORE_SUFFIX)))
                                    {
                                    memset(Log_info.SilenceLevel, '\0', sizeof(Log_info.SilenceLevel));
                                    strcpy(Log_info.SilenceLevel, tag_RMS->value);
                                    }
                                    */
                                /*Capture silence detect prints code start*/
                                silenceTag = NULL;
                                silenceTag = av_dict_get(filt_audio_frame->metadata, "lavfi.silence_start", silenceTag, AV_DICT_IGNORE_SUFFIX);
                                if ((silenceTag != NULL) && (silenceStartIndicatorFlag == 0))
                                {
                                    memset(Log_info.SilenceLevel, '\0', sizeof(Log_info.SilenceLevel));
                                    strcpy(Log_info.SilenceLevel, "YES");

                                    silenceStartIndicatorFlag = 1;
                                    memset(printSilenceMessages, '\0', sizeof(printSilenceMessages));
                                    sprintf(printSilenceMessages,"%s = %s", silenceTag->key, silenceTag->value);
                                    //fprintf(stderr,"Silence start : [%s]\n", printSilenceMessages);
                                }
                                if(silenceStartIndicatorFlag == 1)
                                {
                                    clock_t difference = clock() - blackStartTimer;
                                    milliSec = difference * 1000 / CLOCKS_PER_SEC;

                                    if(milliSec >= 1)
                                    {
                                        gSilenceAlarmFlag = 1;
                                    }
                                }


                                silenceTag1 = NULL;
                                silenceTag1 = av_dict_get(filt_audio_frame->metadata, "lavfi.silence_end", silenceTag1, AV_DICT_IGNORE_SUFFIX);
                                if((silenceTag1 != NULL) && (silenceStartIndicatorFlag == 1))
                                {
                                    memset(Log_info.SilenceLevel, '\0', sizeof(Log_info.SilenceLevel));
                                    strcpy(Log_info.SilenceLevel, "NO");

                                    /*      fprintf(stderr,"\n[%s]\n", printSilenceMessages);
                                     *                                                                      fprintf(stderr,"[%s = %s]\n", silenceTag1->key, silenceTag1->value); */
                                    sprintf(silenceBuffer,"%s\t%s = %s\n",printSilenceMessages, silenceTag1->key, silenceTag1->value);
                                    silenceStartIndicatorFlag = 0;
                                    gSilenceAlarmFlag = 0;
                                    //fprintf(stderr,"Sending silence buffer : [%s]\n", silenceBuffer);
                                    /*sendto(sockfd, (const char *)silenceBuffer, strlen(silenceBuffer),
                                      MSG_CONFIRM, (const struct sockaddr *) &servaddr,
                                      sizeof(servaddr));*/
                                }
                                /*Capture silence detect prints code end*/

                                av_frame_unref(filt_audio_frame);
                            }
                            av_frame_unref(frame_audio);
                        }
                    }
                }/*Audio ends*/
            }//Audio check
        } 
       // av_packet_unref(&packet);//Commented on 01/04/2022 Anil
    }
    /* flush the dec */
    packet->data = NULL;
    packet->size = 0;
    av_packet_unref(packet);

    if (output_file)
        fclose(output_file);

end:
    avfilter_graph_free(&video_filter_graph);
    avcodec_free_context(&dec_video_ctx);
    avfilter_graph_free(&filter_audio_graph);
    avcodec_free_context(&dec_audio_ctx);
    avformat_close_input(&fmt_ctx);
    av_packet_free(&packet);
    av_buffer_unref(&hw_device_ctx);

    av_frame_free(&frame_video);
//    av_frame_free(&tmp_frame);
    av_frame_free(&filt_audio_frame);
    av_frame_free(&filt_video_frame);
    av_frame_free(&frame_audio);
    av_frame_free(&sw_frame);


    if (ret < 0 && ret != AVERROR_EOF)
    {
        fprintf(stderr,"Value of ret : [%d]\n", ret);
        fprintf(stderr, "Error occurred in hw.c: %s\n", av_err2str(ret));
        exit(1);
    }
    return 0;
}

void* hw(void *arg) {
    while(1){
        if(hwvideo_pid && No_audpid){
            printf("\nHARDWARE THREAD INVOKED \n");
            //	printf("\nHello, hardware!: vedio_pid=%d", hwvideo_pid);
            for(i=0;i<No_audpid;i++)
            {
                //	printf("\nHello, hardware!: aud_pid=%d", hwaud_pid[i]);
            }
            //printf("gMULTICAST_IP: %s\n",gMULTICAST_IP);
            hardware();
            return (void*)0;
        }
    }
    return (void*)0;
}
