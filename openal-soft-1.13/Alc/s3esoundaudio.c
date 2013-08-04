
#include "config.h"

#include <malloc.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include <s3eSound.h>
#include <s3eThread.h>


static const ALCchar s3eDevice[] = "s3eSound";

typedef struct {
    int channel;

    ALubyte*                mix_data;
    volatile int            killNow;

    volatile ALvoid*        thread;     // Has to be volatile, it can change in different threads
    s3eSoundGenAudioInfo*   thread_info;
    s3eThreadSem*           thread_semaphore;
    volatile int            thread_finishedWorking;
    volatile int            thread_exited;
} s3e_data;


static ALuint s3e_channel_thread(ALvoid *ptr)
{
    ALCdevice *Device = (ALCdevice*)ptr;
    s3e_data *data = (s3e_data*)Device->ExtraData;
    data->thread_exited = 0;

    // It is very important that this thread reacts ASAP to
    // signal from s3e callback, so the main s3e thread is
    // not paused for too long.
    // Luckily, we can use semaphore to wait for signal from
    // s3e callback function, that way we react immediately.

    while( !data->killNow )
    {
        // Wait for s3e main thread to signal us via semaphore
        if( s3eThreadSemWait(data->thread_semaphore, 10) != S3E_RESULT_SUCCESS ) {
            // Waited 10ms, but without any signal - just check if we need
            // to close this thread, and then wait again
            continue;
        }
        assert(data->thread_finishedWorking == 0);

        // Mix the data... Now OpenAL can lock its mutex without Marmalade reporting error
        aluMixData(Device, data->thread_info->m_Target, data->thread_info->m_NumSamples);

        // Signal to s3e callback function that we're finished with mixing the data
        // We have to use volatile varialble instead of semaphore or lock (mutex)
        // because Marmalade doesn't allow locking of the s3e thread
        data->thread_finishedWorking = 1;
    }

    data->thread_exited = 1;
    return 0;
}


int32 s3e_more_audio(void* systemData, void* userData)
{
    // This code assumes that s3e_more_audio won't be called while
    // we're already in it.
    // That is because this function is called only from the
    // s3e main thread, and it has to wait for us to exit this 
    // function before it can call it again

    ALCdevice *pDevice = (ALCdevice*)userData;
    s3eSoundGenAudioInfo* info = (s3eSoundGenAudioInfo*)systemData;
    s3e_data* data;

    if( systemData == NULL || userData == NULL ) return 0;
    data = (s3e_data*)pDevice->ExtraData;

    // Check if this channel is actually closed or should be closed
    if( data == NULL || data->killNow || data->thread == NULL || data->thread_exited ) {
        info->m_EndSample = S3E_TRUE;
        return 0;
    }

    // We can't call aluMixData() directly, because in it it calls mutex lock
    // and s3e doesn't allow callback functions to lock the main s3e thread
    //aluMixData(pDevice, info->m_Target, info->m_NumSamples);

    // So set up worker thread data
    data->thread_info = info;
    data->thread_finishedWorking = 0;

    // Signal our thread that it has work waiting for it
    // We can use semaphore in this case, because we will not block this thread
    // Worker thread will act immediately.
    s3eThreadSemPost(data->thread_semaphore);

    // Wait for worker thread to finish with work
    // And we can't wait with our semaphore... because it would block s3e thread and that's not allowed
    // So we have to use volatile variable and Sleep() instead.
    while( data->thread && data->thread_finishedWorking == 0 ) Sleep(0);

    return info->m_NumSamples;
}


static ALCboolean s3e_open_playback(ALCdevice *device, const ALCchar *deviceName) {
    if( !deviceName ) {
        deviceName = s3eDevice;
    } 
    if( strcmp(s3eDevice, deviceName) == 0 ) {
        s3e_data* data = (s3e_data*)malloc(sizeof(s3e_data));
        data->channel = s3eSoundGetFreeChannel();
        data->mix_data = NULL;
        data->killNow = 0;
        data->thread_semaphore = NULL;
        data->thread_finishedWorking = 0;
        data->thread_exited = 0;
        device->ExtraData = data;
        device->szDeviceName = strdup(deviceName);
        device->FmtType = DevFmtShort;

        if( s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) ) {
          device->FmtChans = DevFmtStereo;
        } else {
          device->FmtChans = DevFmtMono;
        }
        // when generating sound, channel frequency is ignored
        device->Frequency = s3eSoundGetInt(S3E_SOUND_OUTPUT_FREQ);

        return ALC_TRUE;
    } else {
        return ALC_FALSE;
    }
}


static void s3e_close_playback(ALCdevice *device) {
    s3e_data *data = (s3e_data*)device->ExtraData;
    s3eSoundChannelStop(data->channel);
    free(data);
    device->ExtraData = NULL;
}

static ALCboolean s3e_reset_playback(ALCdevice *device) {
    s3e_data *data = (s3e_data*)device->ExtraData;
    int dataSize = device->UpdateSize * FrameSizeFromDevFmt(device->FmtChans, device->FmtType);

    data->mix_data = (ALubyte*)calloc(1, dataSize * sizeof(ALubyte));
    if(!data->mix_data) {
        AL_PRINT("buffer malloc failed\n");
        return ALC_FALSE;
    }
    SetDefaultWFXChannelOrder(device);

    // Create semaphore we will use to signal worker thread
    data->thread_semaphore = s3eThreadSemCreate(0);

    // Start worker thread
    data->killNow = 0;
    data->thread = StartThread(s3e_channel_thread, device);
    if(data->thread == NULL)
    {
        free(data->mix_data);
        data->mix_data = NULL;
        s3eThreadSemDestroy(data->thread_semaphore);
        return ALC_FALSE;
    }

    // Register & start callback functions
    s3eSoundChannelRegister(data->channel, S3E_CHANNEL_GEN_AUDIO, s3e_more_audio, device);
    s3eSoundChannelRegister(data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO, s3e_more_audio, device);

    // Starting infinite playback cycle with any data
    s3eSoundChannelPlay(data->channel, (int16*)data->mix_data, dataSize, 0, 0);
    return ALC_TRUE;
}

static void s3e_stop_playback(ALCdevice *device) {
    s3e_data *data = (s3e_data*)device->ExtraData;
    int i;

    // Signal the thread that it has to close
    data->killNow = 1;

    // Stop the callback functions
    s3eSoundChannelUnRegister(data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO);
    s3eSoundChannelUnRegister(data->channel, S3E_CHANNEL_GEN_AUDIO);

    // Stop the thread
    if(data->thread) {
        ALvoid *thread = (ALvoid*)data->thread;
        data->thread = NULL;    // Remove the pointer to thread before stopping it

        // Wait for thread to exit gracefully, but don't block indefinitely
        for( i=0; i<20 && !data->thread_exited; i++ ) Sleep(2);
        // And now really stop the thread
        StopThread( thread );
    }

    // Destroy the semaphore
    if(data->thread_semaphore != NULL) s3eThreadSemDestroy(data->thread_semaphore);

    s3eSoundChannelStop(data->channel);
    if( data->mix_data != NULL ) {
        free(data->mix_data);
        data->mix_data = NULL;
    }
}

static ALCboolean s3e_open_capture(ALCdevice *pDevice, const ALCchar *deviceName) {
    // maybe one day
    (void)pDevice;
    (void)deviceName;
    return ALC_FALSE;
}

BackendFuncs s3e_funcs = {
    s3e_open_playback,
    s3e_close_playback,
    s3e_reset_playback,
    s3e_stop_playback,
    s3e_open_capture,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

void alc_s3e_init(BackendFuncs *func_list) {
    *func_list = s3e_funcs;
}
    
void alc_s3e_deinit(void) {

}

void alc_s3e_probe(int type) {
    if(type == DEVICE_PROBE)
    {
        AppendDeviceList(s3eDevice);
    }
    else if(type == ALL_DEVICE_PROBE)
    {
        AppendAllDeviceList(s3eDevice);
    }
}

