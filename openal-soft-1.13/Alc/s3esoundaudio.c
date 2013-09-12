
#include "config.h"

#include <malloc.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include <s3eSound.h>
#include <s3eThread.h>
#include <s3eDevice.h>
#include <s3eTimer.h>


static const ALCchar s3eDevice[] = "s3eSound";

typedef struct _s3e_data {
    // s3e sound channel ID
    int                     channel;
    // Buffer used to play sound with s3e
    ALubyte*                mix_data;
    // Size of single sample mixed in advance
    int                     sampleSize;
    // Bytes per sound sample - we use 2 or 4 bytes
    // depending if we play mono or stereo sound
    int                     bytesPerSample;
    // If true, we're playing stereo sound
    int                     isStereo;

    // Pre-buffer data (cyclic or ring buffer)
    // Stereo sound - use uint32
    ALubyte*                preBufferData;
    // Maximum size of pre-buffer data
    int                     preBufferSize;
    // Pointer where is start of usable data
    volatile int            preBuffer_DataStartIndex;
    // How much usable (already mixed) data is in the buffer
    volatile int            preBuffer_DataLength;
    // If true, it means that data in pre-buffer is ready
    // to be send to sound channel
    volatile int            preBuffer_IsDataReady;

    // Pointer to worker thread
    // It has to be volatile, it can change in different threads
    volatile ALvoid*        thread;
    // If true, we should immediately exit worker thread
    volatile int            killNow;
    // Semaphore used for synchronization between 
    // s3e callback function and worker thread
    s3eThreadSem*           thread_semaphore;
    // If true, worker thread is closed
    volatile int            thread_exited;
} s3e_data;


static ALuint s3e_channel_thread( ALvoid *ptr )
{
    ALCdevice *Device = (ALCdevice*)ptr;
    s3e_data *data = (s3e_data*)Device->ExtraData;
    int samplesToMix, idx1, len1, len2;
    data->thread_exited = 0;

    // This thread will pre-mix sound data into internal buffer
    // so when s3e sound callback is called, he just has to 
    // copy data from that buffer... And then we get signal
    // to pre-mix some more data for next s3e sound callback.

    // It is very important that this thread reacts ASAP to
    // signal from s3e callback, so the main s3e thread is
    // not paused for too long.
    // Luckily, we can use semaphore to wait for signal from
    // s3e callback function, that way we react almost immediately.

    while( !data->killNow && !s3eDeviceCheckQuitRequest() )
    {
        assert( data->preBuffer_IsDataReady == 0 );

        if( data->preBuffer_DataLength == 0 ) data->preBuffer_DataStartIndex = 0;

        // Mix the data in the circular (ring) buffer
        //samplesToMix = data->preBufferSize - data->preBuffer_DataLength;    // Always fill buffer to the max
        samplesToMix = min( data->sampleSize, data->preBufferSize - data->preBuffer_DataLength );

        // Mix data in two steps - first mix from the end of the buffer
        idx1 = (data->preBuffer_DataStartIndex + data->preBuffer_DataLength);
        if( idx1 >= data->preBufferSize ) idx1 -= data->preBufferSize;
        len1 = min( data->preBufferSize - idx1, samplesToMix );
        if( len1 > 0 ) aluMixData( Device, data->preBufferData + idx1*data->bytesPerSample, len1 );
        // Mix 2nd step - from start of the buffer
        len2 = samplesToMix - len1;
        if( len2 > 0 ) aluMixData( Device, data->preBufferData, len2 );
        assert( len2 <= data->preBuffer_DataStartIndex );


        // Ok, sound data mixed
        data->preBuffer_DataLength += samplesToMix;

        // Mark it that we have data ready
        data->preBuffer_IsDataReady = 1;

        // Wait for s3e main thread to signal us via semaphore
        // before we start mixing again
        s3eDeviceYield(0);
        while( !data->killNow && s3eThreadSemWait(data->thread_semaphore, 10) != S3E_RESULT_SUCCESS )
        {
            s3eDeviceYield(0);

            // Waited 10ms, but without any signal - just check if we need
            // to close this thread, and then wait again
            if( s3eDeviceCheckQuitRequest() ) data->killNow = 1;

            // Wait another 10ms for signal from s3e callback
        }
    }

    data->thread_exited = 1;
    return 0;
}


int32 s3e_more_audio( void* systemData, void* userData )
{
    // This code assumes that the function s3e_more_audio 
    // won't be called while we're already in it.
    // That is because this function is called only from the
    // s3e main thread, and it has to wait for us to exit this 
    // function before it can call it again
    // NOTE: Inside this function we can't call s3eDeviceYield()
    // or any other sleep function, because if we do, s3e sound
    // callback 

    int dataWritten, len1, len2;
    uint64 startTime;
    ALCdevice *pDevice = (ALCdevice*)userData;
    s3eSoundGenAudioInfo* info = (s3eSoundGenAudioInfo*)systemData;
    s3e_data* data;

    if( systemData == NULL || userData == NULL ) return 0;
    data = (s3e_data*)pDevice->ExtraData;

    assert( (data->isStereo!=0) == (info->m_Stereo!=0) );

    // Check if this channel is actually closed or should be closed
    if( data == NULL || data->killNow || data->thread == NULL || data->thread_exited )
    {
        info->m_EndSample = S3E_TRUE;
        // If we would return 0 - as we should - s3e will not detect that
        // application is closed and will call our function again immediately.
        // So we return info->m_NumSamples so application will be closed properly.
        // We clear output sound buffer so it would at least play silence.
        //memset( info->m_Target, 0, info->m_NumSamples * data->bytesPerSample );
        return info->m_NumSamples;
        // We can't return 0 even though the app should be closed.
        // It would actually crash the app as s3e will delete 
        // sound buffer in next call, and then again call us
        // which would mean that functions params systemData
        // and userData would have invalid values!
        //return 0;
    }


    // Check if mixed data is ready from worker thread
    startTime = s3eTimerGetMs();
    while( !data->preBuffer_IsDataReady ) 
    {
        // No data is ready; but we CAN'T use s3eDeviceYield() nor Sleep()
        // because that could call s3e_more_audio while we're already in it
        // and that would break the code
        //s3eDeviceYield(0);

        // If we are waiting for worker thread to finish mixing
        // for more than 20ms, it most probably means that application
        // is paused (suspended) or has received terminate signal
        // So don't wait any more
        if( s3eTimerGetMs() - startTime > 20 )
        {
            // We HAVE to return info->m_NumSamples instead of 0
            // even though we haven't mixed any data. This is because
            // Marmalade doesn't detect that application is paused/closed
            // and will keep calling our callback function until it gets
            // all the data it required.
            // So at least clear output sound buffer so it would play silence.
            //memset( info->m_Target, 0, info->m_NumSamples * data->bytesPerSample );
            return info->m_NumSamples;
            //return 0; // We can't return 0 - it would block the application
        }

        // Check if application has received quit request or is paused
        // Unfortunately, this doesn't always work - that's why we have
        // timer above which will exit this function if more than 20ms passed
        // If any of those two functions return true, it means that
        // Marmalade HAS recognized that it should be paused/terminated
        // so we can properly return 0.
        if( s3eDeviceCheckQuitRequest() || s3eDeviceCheckPauseRequest() ) return 0;
    }

    // Copy new data (note: data->preBufferData is a cyclic buffer)
    dataWritten = min( info->m_NumSamples, data->preBuffer_DataLength );
    if( data->preBuffer_DataStartIndex + dataWritten <= data->preBufferSize )
    {
        // Copy all data in 1 step
        memcpy( info->m_Target, data->preBufferData + data->preBuffer_DataStartIndex*data->bytesPerSample, dataWritten * data->bytesPerSample );
    } else {
        // Copy data in 2 steps
        len1 = data->preBufferSize - data->preBuffer_DataStartIndex;
        memcpy( info->m_Target, data->preBufferData + data->preBuffer_DataStartIndex*data->bytesPerSample, len1 * data->bytesPerSample );
        len2 = dataWritten - len1;
        if( len2 > 0 ) memcpy( info->m_Target, data->preBufferData, len2 * data->bytesPerSample );
    }
    data->preBuffer_DataStartIndex += dataWritten;
    if( data->preBuffer_DataStartIndex >= data->preBufferSize ) data->preBuffer_DataStartIndex -= data->preBufferSize;
    data->preBuffer_DataLength -= dataWritten;

    data->preBuffer_IsDataReady = 0;

    // Signal to worker thread that it can pre-mix next piece of sound buffer
    // We can use semaphore in this case, because we will not block this thread
    // Worker thread will act immediately.
    s3eThreadSemPost(data->thread_semaphore);

    return dataWritten;
}


static ALCboolean s3e_open_playback( ALCdevice *device, const ALCchar *deviceName )
{
    s3e_data* data;

    if( !deviceName ) deviceName = s3eDevice;
    if( strcmp(s3eDevice, deviceName) != 0 ) return ALC_FALSE;

    data = (s3e_data*)malloc(sizeof(s3e_data));
    data->channel = s3eSoundGetFreeChannel();
    data->mix_data = NULL;
    data->sampleSize = 0;
    data->bytesPerSample = 0;
    data->preBufferData = NULL;
    data->preBufferSize = 0;
    data->preBuffer_DataStartIndex = 0;
    data->preBuffer_DataLength = 0;
    data->thread = NULL;
    data->killNow = 0;
    data->thread_semaphore = NULL;
    data->thread_exited = 0;
    device->ExtraData = data;
    device->szDeviceName = strdup(deviceName);
    device->FmtType = DevFmtShort;  // 16 bit per channel

    // Register callback functions
    s3eSoundChannelRegister( data->channel, S3E_CHANNEL_GEN_AUDIO, s3e_more_audio, device );

    // Check if we have stereo sound
    if( s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) )
    {
        data->isStereo = s3eSoundChannelRegister( data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO, s3e_more_audio, device ) == S3E_RESULT_SUCCESS;
    } else data->isStereo = 0;
    device->FmtChans = data->isStereo ? DevFmtStereo : DevFmtMono;

    // when generating sound, channel frequency is ignored - we use output device frequency
    device->Frequency = s3eSoundGetInt(S3E_SOUND_OUTPUT_FREQ);

    return ALC_TRUE;
}


static void s3e_close_playback( ALCdevice *device )
{
    s3e_data *data = (s3e_data*)device->ExtraData;
    device->ExtraData = NULL;
    s3eSoundChannelStop(data->channel);
    free(data);
}

static ALCboolean s3e_reset_playback( ALCdevice *device )
{
    s3e_data *data = (s3e_data*)device->ExtraData;
    data->sampleSize = device->UpdateSize;
    data->bytesPerSample = FrameSizeFromDevFmt(device->FmtChans, device->FmtType) * sizeof(ALubyte);
    assert( data->bytesPerSample == (data->isStereo ? 4 : 2) );

    data->mix_data = (ALubyte*)calloc(1, data->sampleSize * data->bytesPerSample);
    if( !data->mix_data ) {
        AL_PRINT("buffer malloc failed\n");
        return ALC_FALSE;
    }
    SetDefaultWFXChannelOrder(device);

    // Create semaphore we will use to signal worker thread
    data->thread_semaphore = s3eThreadSemCreate(0);
    // Create pre-buffer & initialize data
    data->preBufferSize = data->sampleSize * 2;
    data->preBufferData = (ALubyte*)calloc( 1, data->preBufferSize * data->bytesPerSample );
    data->preBuffer_DataStartIndex = data->preBuffer_DataLength = 0;
    data->preBuffer_IsDataReady = 0;

    // Start worker thread
    data->killNow = 0;
    data->thread = StartThread( s3e_channel_thread, device );
    if( data->thread == NULL )
    {
        free(data->mix_data);
        data->mix_data = NULL;
        free(data->preBufferData);
        data->preBufferData = NULL;
        s3eThreadSemDestroy(data->thread_semaphore);
        return ALC_FALSE;
    }

    // Starting infinite playback cycle with any data
    //s3eSoundChannelPlay( data->channel, (int16*)data->mix_data, data->sampleSize * data->bytesPerSample, 0, 0 );
    s3eSoundChannelPlay( data->channel, (int16*)data->mix_data, data->sampleSize * (data->isStereo ? 2 : 1), 0, 0 );
    return ALC_TRUE;
}

static void s3e_stop_playback( ALCdevice *device )
{
    s3e_data *data = (s3e_data*)device->ExtraData;
    int i;

    // Signal the thread that it has to close
    data->killNow = 1;

    // Stop the callback functions
    s3eSoundChannelStop(data->channel);
    s3eSoundChannelUnRegister( data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO );
    s3eSoundChannelUnRegister( data->channel, S3E_CHANNEL_GEN_AUDIO );

    // Stop the thread
    if( data->thread ) {
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
    if( data->preBufferData != NULL ) {
        free(data->preBufferData);
        data->preBufferData = NULL;
    }
}

static ALCboolean s3e_open_capture( ALCdevice *pDevice, const ALCchar *deviceName )
{
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

