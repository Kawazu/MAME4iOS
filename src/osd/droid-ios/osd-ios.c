//============================================================
//
//  myosd.c - Implementation of osd stuff
//
//  Copyright (c) 1996-2007, Nicola Salmoria and the MAME Team.
//  Visit http://mamedev.org for licensing and usage restrictions.
//
//  MAME4DROID MAME4iOS by David Valdeita (Seleuco)
//
//============================================================

#include "emu.h"
#include "myosd.h"

#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#import <AudioToolbox/AudioQueue.h>
#import <AudioToolbox/AudioToolbox.h>

/* Audio Resources */
//minimum required buffers for iOS AudioQueue

#define AUDIO_BUFFERS 3

int  myosd_fps = 1;
int  myosd_inGame = 0;
int  myosd_display_width;
int  myosd_display_height;
int  myosd_in_menu = 0;
int  myosd_force_pxaspect = 0;

int  myosd_filter_clones = 0;
int  myosd_filter_not_working = 0;

int  myosd_hiscore=1;
int  myosd_speed = 100;

static int lib_inited = 0;
static int soundInit = 0;

const char * myosd_version = build_version;


typedef struct AQCallbackStruct {
    AudioQueueRef queue;
    UInt32 frameCount;
    AudioQueueBufferRef mBuffers[AUDIO_BUFFERS];
    AudioStreamBasicDescription mDataFormat;
} AQCallbackStruct;

AQCallbackStruct in;

static pthread_mutex_t sound_mutex     = PTHREAD_MUTEX_INITIALIZER;

static int global_low_latency_sound  = 1;

int sound_close_AudioQueue(void);
int sound_open_AudioQueue(int rate, int bits, int stereo);
int sound_close_AudioUnit(void);
int sound_open_AudioUnit(int rate, int bits, int stereo);
void queue(unsigned char *p,unsigned size);
unsigned short dequeue(unsigned char *p,unsigned size);
inline int emptyQueue(void);

static myosd_callbacks host_callbacks;
extern int ios_main(int argc, char**argv);  // in osdmain.c

// main LIBMAIN entry point, setup callbacks and then call main in osdmain
int myosd_main(int argc, char** argv, myosd_callbacks* callbacks, size_t callbacks_size)
{
    // video_init, video_draw, and input_poll are required
    memcpy(&host_callbacks, callbacks, sizeof(host_callbacks));
    if (argc == 0) {
        static char* args[] = {"libmame"};
        argc = 1;
        argv = args;
    }
    return ios_main(argc, argv);
}

// myosd_get
intptr_t myosd_get(int var)
{
    switch (var)
    {
        case MYOSD_VERSION:
            return 139;
            
        case MYOSD_VERSION_STRING:
            return (intptr_t)(void*)build_version;
            
        case MYOSD_STATE:
            return (myosd_inGame  ? MYOSD_STATE_INGAME : 0) |
                   (myosd_in_menu ? MYOSD_STATE_INMENU : 0) |
                   ((myosd_in_menu==2) ? MYOSD_STATE_CONFIGURE_INPUT : 0);
            
        case MYOSD_FPS:
            return myosd_fps;
            
        case MYOSD_SPEED:
            return myosd_speed;
            
        case MYOSD_HISCORE:
            return myosd_hiscore;

        case MYOSD_FORCE_PIXEL_ASPECT:
            return myosd_force_pxaspect;
    }
    return 0;
}

// myosd_set
void myosd_set(int var, intptr_t value)
{
    switch (var)
    {
        case MYOSD_DISPLAY_WIDTH:
            myosd_display_width = value;
            break;
        case MYOSD_DISPLAY_HEIGHT:
            myosd_display_height = value;
            break;
        case MYOSD_GAME_FILTER:
            myosd_filter_not_working = (value & MYOSD_FILTER_NOTWORKING) != 0;
            myosd_filter_clones = (value & MYOSD_FILTER_CLONES) != 0;
            break;
        case MYOSD_FPS:
            myosd_fps = value;
            break;
        case MYOSD_SPEED:
            myosd_speed = value;
            break;
        case MYOSD_HISCORE:
            myosd_hiscore = value;
            break;
        case MYOSD_FORCE_PIXEL_ASPECT:
            myosd_force_pxaspect = value;
            break;
    }
}


void myosd_set_video_mode(int width,int height)
{
    mame_printf_debug("myosd_set_video_mode: %dx%d\n",width,height);
    host_callbacks.video_init(width,height);
}

void myosd_video_draw(render_primitive* prims, int width, int height)
{
    host_callbacks.video_draw((myosd_render_primitive*)prims, width, height);
}

// output channel callback, send output "up" to the app via myosd_output
static void myosd_output(void *param, const char *format, va_list argptr)
{
    if (host_callbacks.output_text != NULL)
    {
        char buffer[1204];
        vsnprintf(buffer, sizeof(buffer)-1, format, argptr);
        host_callbacks.output_text((int)(intptr_t)param, buffer);
    }
}

void myosd_poll_input(myosd_input_state* input)
{
    host_callbacks.input_poll(input, sizeof(myosd_input_state));
}

extern void myosd_set_game_info(const game_driver *info[], int game_count)
{
    if (host_callbacks.set_game_info != NULL)
        host_callbacks.set_game_info((myosd_game_info**)info, game_count);
}

void myosd_init(void)
{
	int res = 0;
    
    // capture all MAME output so we can send it to the app.
    for (int n=0; n<OUTPUT_CHANNEL_COUNT; n++)
        mame_set_output_channel((output_channel)n, myosd_output, (void*)n, NULL, NULL);

	if (!lib_inited )
    {
       mame_printf_debug("myosd_init\n");

       lib_inited = 1;
    }
}

void myosd_deinit(void)
{
    if (lib_inited )
    {
        mame_printf_debug("myosd_deinit\n");

    	lib_inited = 0;
    }
}

void myosd_closeSound(void) {
    
    if (host_callbacks.sound_exit != NULL)
        return host_callbacks.sound_exit();
    
	if( soundInit == 1 )
	{
        mame_printf_debug("myosd_closeSound\n");

		
        if(global_low_latency_sound)
           sound_close_AudioUnit();
        else
           sound_close_AudioQueue();  

	   	soundInit = 0;
	}
}

void myosd_openSound(int rate,int stereo) {
    
    if (host_callbacks.sound_init != NULL)
        return host_callbacks.sound_init(rate, stereo);
    
	if( soundInit == 0)
	{
        if(global_low_latency_sound)
        {
            mame_printf_debug("myosd_openSound LOW LATENCY rate:%d stereo:%d \n",rate,stereo);
            sound_open_AudioUnit(rate, 16, stereo);
        }
        else
        {
            mame_printf_debug("myosd_openSound NORMAL rate:%d stereo:%d \n",rate,stereo);
            sound_open_AudioQueue(rate, 16, stereo);
        }
       
		soundInit = 1;
	}
}

void myosd_sound_play(void *buff, int len)
{
    if (host_callbacks.sound_play != NULL)
        return host_callbacks.sound_play(buff, len);

	queue((unsigned char *)buff,len);
}

//SQ buffers for sound between MAME and iOS AudioQueue. AudioQueue
//SQ callback reads from these.
//SQ Size: (48000/30fps) * bytesize * stereo * (3 buffers)
#define TAM (1600 * 2 * 2 * 3)
unsigned char ptr_buf[TAM];
unsigned head = 0;
unsigned tail = 0;

inline int fullQueue(unsigned short size){

    if(head < tail)
	{
		return head + size >= tail;
	}
	else if(head > tail)
	{
		return (head + size) >= TAM ? (head + size)- TAM >= tail : false;
	}
	else return false;
}

inline int emptyQueue(){
	return head == tail;
}

void queue(unsigned char *p,unsigned size){
        unsigned newhead;
		if(head + size < TAM)
		{
			memcpy(ptr_buf+head,p,size);
			newhead = head + size;
		}
		else
		{
			memcpy(ptr_buf+head,p, TAM -head);
			memcpy(ptr_buf,p + (TAM-head), size - (TAM-head));
			newhead = (head + size) - TAM;
		}
		pthread_mutex_lock(&sound_mutex);

		head = newhead;

		pthread_mutex_unlock(&sound_mutex);
}

unsigned short dequeue(unsigned char *p,unsigned size){

    	unsigned real;
    	unsigned datasize;

		if(emptyQueue())
		{
	    	memset(p,0,size);//TODO ver si quito para que no petardee
			return size;
		}

		pthread_mutex_lock(&sound_mutex);

		datasize = head > tail ? head - tail : (TAM - tail) + head ;
		real = datasize > size ? size : datasize;

		if(tail + real < TAM)
		{
			memcpy(p,ptr_buf+tail,real);
			tail+=real;
		}
		else
		{
			memcpy(p,ptr_buf + tail, TAM - tail);
			memcpy(p+ (TAM-tail),ptr_buf , real - (TAM-tail));
			tail = (tail + real) - TAM;
		}

		pthread_mutex_unlock(&sound_mutex);

        return real;
}

void checkStatus(OSStatus status){}


static void AQBufferCallback(void *userdata,
							 AudioQueueRef outQ,
							 AudioQueueBufferRef outQB)
{
	unsigned char *coreAudioBuffer;
	coreAudioBuffer = (unsigned char*) outQB->mAudioData;

	dequeue(coreAudioBuffer, in.mDataFormat.mBytesPerFrame * in.frameCount);
	outQB->mAudioDataByteSize = in.mDataFormat.mBytesPerFrame * in.frameCount;

	AudioQueueEnqueueBuffer(outQ, outQB, 0, NULL);
}


int sound_close_AudioQueue(){

	if( soundInit == 1 )
	{
		AudioQueueDispose(in.queue, true);
		soundInit = 0;
        head = 0;
        tail = 0;
	}
	return 1;
}

int sound_open_AudioQueue(int rate, int bits, int stereo){

    Float64 sampleRate = 48000.0;
    int i;
    UInt32 err;
    int fps;
    int bufferSize;

    if(rate==44100)
    	sampleRate = 44100.0;
    if(rate==32000)
    	sampleRate = 32000.0;
    else if(rate==22050)
    	sampleRate = 22050.0;
    else if(rate==11025)
    	sampleRate = 11025.0;

	//SQ Roundup for games like Galaxians
    //fps = ceil(Machine->drv->frames_per_second);
    fps = 60;//TODO

    if( soundInit == 1 )
    {
    	sound_close_AudioQueue();
    }

    soundInit = 0;
    memset (&in.mDataFormat, 0, sizeof (in.mDataFormat));
    in.mDataFormat.mSampleRate = sampleRate;
    in.mDataFormat.mFormatID = kAudioFormatLinearPCM;
    in.mDataFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger  | kAudioFormatFlagIsPacked;
    in.mDataFormat.mBytesPerPacket =  (stereo == 1 ? 4 : 2 );
    in.mDataFormat.mFramesPerPacket = 1;
    in.mDataFormat.mBytesPerFrame = (stereo ==  1? 4 : 2);
    in.mDataFormat.mChannelsPerFrame = (stereo == 1 ? 2 : 1);
    in.mDataFormat.mBitsPerChannel = 16;
	in.frameCount = rate / fps;

    err = AudioQueueNewOutput(&in.mDataFormat,
							  AQBufferCallback,
							  NULL,
							  NULL,
							  kCFRunLoopCommonModes,
							  0,
							  &in.queue);

    //printf("res %ld",err);

    bufferSize = in.frameCount * in.mDataFormat.mBytesPerFrame;

	for (i=0; i<AUDIO_BUFFERS; i++)
	{
		err = AudioQueueAllocateBuffer(in.queue, bufferSize, &in.mBuffers[i]);
		in.mBuffers[i]->mAudioDataByteSize = bufferSize;
		AudioQueueEnqueueBuffer(in.queue, in.mBuffers[i], 0, NULL);
	}

	soundInit = 1;
	err = AudioQueueStart(in.queue, NULL);

	return 0;

}

///////// AUDIO UNIT
#define kOutputBus 0
static AudioComponentInstance audioUnit;

static OSStatus playbackCallback(void *inRefCon,
                                 AudioUnitRenderActionFlags *ioActionFlags,
                                 const AudioTimeStamp *inTimeStamp,
                                 UInt32 inBusNumber,
                                 UInt32 inNumberFrames,
                                 AudioBufferList *ioData) {
    // Notes: ioData contains buffers (may be more than one!)
    // Fill them up as much as you can. Remember to set the size value in each buffer to match how
    // much data is in the buffer.
    
	unsigned  char *coreAudioBuffer;
    
    int i;
    for (i = 0 ; i < ioData->mNumberBuffers; i++)
    {
        coreAudioBuffer = (unsigned char*) ioData->mBuffers[i].mData;
        //ioData->mBuffers[i].mDataByteSize = dequeue(coreAudioBuffer,inNumberFrames * 4);
        dequeue(coreAudioBuffer,inNumberFrames * 4);
        ioData->mBuffers[i].mDataByteSize = inNumberFrames * 4;
    }
    
    return noErr;
}

int sound_close_AudioUnit(){
    
	if( soundInit == 1 )
	{
		OSStatus status = AudioOutputUnitStop(audioUnit);
		checkStatus(status);
        
		AudioUnitUninitialize(audioUnit);
		soundInit = 0;
        head = 0;
        tail = 0;
	}
    
	return 1;
}

int sound_open_AudioUnit(int rate, int bits, int stereo){
    Float64 sampleRate = 48000.0;

    if( soundInit == 1 )
    {
        sound_close_AudioUnit();
    }
    
    if(rate==44100)
        sampleRate = 44100.0;
    if(rate==32000)
        sampleRate = 32000.0;
    else if(rate==22050)
        sampleRate = 22050.0;
    else if(rate==11025)
        sampleRate = 11025.0;
    
    //audioBufferSize =  (rate / 60) * 2 * (stereo==1 ? 2 : 1) ;
    
    OSStatus status;
    
    // Describe audio component
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    
    // Get component
    AudioComponent inputComponent = AudioComponentFindNext(NULL, &desc);
    
    // Get audio units
    status = AudioComponentInstanceNew(inputComponent, &audioUnit);
    checkStatus(status);
    
    UInt32 flag = 1;
    // Enable IO for playback
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Output,
                                  kOutputBus,
                                  &flag,
                                  sizeof(flag));
    checkStatus(status);
    
    AudioStreamBasicDescription audioFormat;
    
    memset (&audioFormat, 0, sizeof (audioFormat));
    
    audioFormat.mSampleRate = sampleRate;
    audioFormat.mFormatID = kAudioFormatLinearPCM;
    audioFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger  | kAudioFormatFlagIsPacked;
    audioFormat.mBytesPerPacket =  (stereo == 1 ? 4 : 2 );
    audioFormat.mFramesPerPacket = 1;
    audioFormat.mBytesPerFrame = (stereo ==  1? 4 : 2);
    audioFormat.mChannelsPerFrame = (stereo == 1 ? 2 : 1);
    audioFormat.mBitsPerChannel = 16;
    
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  kOutputBus,
                                  &audioFormat,
                                  sizeof(audioFormat));
    checkStatus(status);
    
    struct AURenderCallbackStruct callbackStruct;
    // Set output callback
    callbackStruct.inputProc = playbackCallback;
    callbackStruct.inputProcRefCon = NULL;
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Global,
                                  kOutputBus,
                                  &callbackStruct,
                                  sizeof(callbackStruct));
    checkStatus(status);
    
    status = AudioUnitInitialize(audioUnit);
    checkStatus(status);
    
    //ARRANCAR
    soundInit = 1;
    status = AudioOutputUnitStart(audioUnit);
    checkStatus(status);
    
    return 1;
}

