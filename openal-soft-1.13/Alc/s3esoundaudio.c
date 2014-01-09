
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
    // (number of int32 for stereo, or int16 for mono sound)
    int                     preBufferSize;
    // Pointer where is start of usable data
    volatile int            preBuffer_DataStartIndex;
    // How much usable (already mixed) data is in the buffer
    volatile int            preBuffer_DataEndIndex;

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

int getDataInBuffer( const int startIndex, const int endIndex, const int size )
{
    return startIndex <= endIndex   // also if equal - it means 0 length
        ? endIndex - startIndex
        : (size - startIndex) + endIndex;
}


static ALuint s3e_channel_thread( ALvoid *ptr )
{
    ALCdevice *Device = (ALCdevice*)ptr;
    s3e_data *data = (s3e_data*)Device->ExtraData;
    int startIdx, samplesToMix, len, idx1, len1, len2;
    data->thread_exited = 0;

    // This thread will pre-mix sound data into internal buffer
    // so when s3e sound callback is called, he just has to 
    // copy data from that buffer... And then we get signal
    // to pre-mix some more data for next s3e sound callback.

    // This thread can only modify preBuffer_DataEndIndex and never preBuffer_DataStartIndex
    while( !data->killNow && !s3eDeviceCheckQuitRequest() )
    {
        startIdx = data->preBuffer_DataStartIndex;
        len = getDataInBuffer( startIdx, data->preBuffer_DataEndIndex, data->preBufferSize );
        assert( startIdx >= 0 && startIdx < data->preBufferSize );
        assert( len >= 0 && len < data->preBufferSize );

        // Mix the data in the circular (ring) buffer
        // Always fill buffer to the max, minus 16 bytes
        // Those 16 bytes we leave is so startIndex never equals endIndex
        samplesToMix = min( 2*data->sampleSize, (data->preBufferSize - len) - 16 );

        // Mix data in two steps - first mix from the end of the buffer
        idx1 = data->preBuffer_DataEndIndex;
        len1 = min( data->preBufferSize - idx1, samplesToMix );
        if( len1 > 0 ) aluMixData( Device, data->preBufferData + idx1*data->bytesPerSample, len1 );
        // Mix 2nd step - from start of the buffer
        len2 = samplesToMix - len1;
        if( len2 > 0 ) aluMixData( Device, data->preBufferData, len2 );
        assert( len2 <= startIdx );

        // Ok, sound data mixed, move endIndex pointer
        idx1 = data->preBuffer_DataEndIndex + samplesToMix;
        if( idx1 >= data->preBufferSize ) idx1 -= data->preBufferSize;
        data->preBuffer_DataEndIndex = idx1;

        // Wait until some of the data in buffer is used
        // by s3e sound callback
        while( !data->killNow && s3eThreadSemWait(data->thread_semaphore, 10) != S3E_RESULT_SUCCESS )
        {
            // Waited 10ms, but without any signal - just check if we need
            // to close this thread, and then wait again
            if( s3eDeviceCheckQuitRequest() ) data->killNow = 1;
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
    // or any other sleep function (Marmalade limitation)

    int dataLen, dataWritten, len1, len2;
    uint64 startTime;
    ALCdevice *pDevice = (ALCdevice*)userData;
    s3eSoundGenAudioInfo* info = (s3eSoundGenAudioInfo*)systemData;
    s3e_data* data;

    if( systemData == NULL || userData == NULL ) return 0;
    data = (s3e_data*)pDevice->ExtraData;

    // Check if this channel is actually closed or should be closed
    if( data == NULL || data->killNow || data->thread == NULL || data->thread_exited )
    {
        data->killNow = 1;
        info->m_EndSample = S3E_TRUE;
        // If we would return 0 - as we should - s3e will not detect that
        // application is closed and will call our function again immediately.
        // So we return info->m_NumSamples so application will be closed properly.
        return info->m_NumSamples;
        // We can't return 0 even though the app should be closed.
        // It would actually crash the app as s3e will delete 
        // sound buffer in next call, and then again call us
        // which would mean that functions params systemData
        // and userData would have invalid values!
        //return 0;
    }

    assert( (data->isStereo!=0) == (info->m_Stereo!=0) );
    assert( data->preBuffer_DataStartIndex >= 0 && data->preBuffer_DataStartIndex < data->preBufferSize );

    // Check how much data is pre-buffered
    dataLen = getDataInBuffer( data->preBuffer_DataStartIndex, data->preBuffer_DataEndIndex, data->preBufferSize );
    if( dataLen == 0 )
    {
        // Do NOT return 0. It can work, but it will freeze on iOS
        // when system alarm is started.
        //return 0;
        return info->m_NumSamples;
    }

    // Copy new data (note: data->preBufferData is a cyclic buffer)
    dataWritten = min( info->m_NumSamples, dataLen );
    if( data->preBuffer_DataStartIndex + dataWritten <= data->preBufferSize )
    {
        // Copy all data in 1 step
        memcpy( info->m_Target, data->preBufferData + data->preBuffer_DataStartIndex*data->bytesPerSample, dataWritten * data->bytesPerSample );
    } else {
        // Copy data in 2 steps
        len1 = data->preBufferSize - data->preBuffer_DataStartIndex;
        memcpy( info->m_Target, data->preBufferData + data->preBuffer_DataStartIndex*data->bytesPerSample, len1 * data->bytesPerSample );
        len2 = dataWritten - len1;
        if( len2 > 0 ) memcpy( info->m_Target + len1 * data->bytesPerSample/2, data->preBufferData, len2 * data->bytesPerSample );
    }

    // Increse startIndex
    len1 = data->preBuffer_DataStartIndex + dataWritten;
    while( len1 >= data->preBufferSize ) len1 -= data->preBufferSize;
    data->preBuffer_DataStartIndex = len1;

    // Notify thread that new data is needed
    s3eThreadSemPost(data->thread_semaphore);

    return dataWritten;
}

void    FixForMutedSound( int channelId )
{
    // Marmalade fix: play silence (empty buffer) before anything
    // else, otherwise streaming of sound might not work
    const int D = 2048;
    int16 *silenceBuffer, i;
    int64 timeOut;

    silenceBuffer = (int16*)calloc( 1, sizeof(int16)*D );
    s3eSoundChannelPlay( channelId, (int16*)silenceBuffer, D/2, 0, 0 );
    s3eDeviceYield(30);
    s3eSoundChannelStop( channelId );
    s3eDeviceYield(10);

    timeOut = s3eTimerGetUST() + 150; // Don't wait more than 150ms
    // Loop wait while silence sound finishes playing
    while( s3eSoundChannelGetInt( channelId, S3E_CHANNEL_STATUS ) == 1 )
    {
        if( s3eTimerGetUST() > timeOut ) {  // Time out, break
            timeOut = 0;
            break;
        }
        s3eDeviceYield(5);
    }

    if( timeOut != 0 ) free(silenceBuffer); // if timed out, don't delete buffer just to be on a safe side
}

static ALCboolean s3e_open_playback( ALCdevice *device, const ALCchar *deviceName )
{
    int freeChannel, tries;
    s3e_data *data;

    if( !deviceName ) deviceName = s3eDevice;
    if( strcmp(s3eDevice, deviceName) != 0 ) return ALC_FALSE;

    tries = 0;
    while( (freeChannel = s3eSoundGetFreeChannel()) == -1 && tries++ < 10 ) {
        s3eDeviceYield(50);
    }
    if( freeChannel == -1 ) return ALC_FALSE;   // Could not set up the channel

    data = (s3e_data*)malloc(sizeof(s3e_data));
    data->channel = freeChannel;
    data->mix_data = NULL;
    data->sampleSize = 0;
    data->bytesPerSample = 0;
    data->preBufferData = NULL;
    data->preBufferSize = 0;
    data->preBuffer_DataStartIndex = 0;
    data->preBuffer_DataEndIndex = 0;
    data->thread = NULL;
    data->killNow = 0;
    data->thread_semaphore = NULL;
    data->thread_exited = 0;

    FixForMutedSound( data->channel );  // Workaround for Marmalade bug

    device->ExtraData = data;
    device->szDeviceName = strdup(deviceName);
    device->FmtType = DevFmtShort;  // 16 bit per channel
    // when generating sound, channel frequency is ignored - we use output device frequency
    device->Frequency = s3eSoundGetInt(S3E_SOUND_OUTPUT_FREQ);

    // This is set experimentally - bigger number means less cracking in the noise but slower reaction to sound play/stop
    // NOTE: Setting this can crash the app when it is resumed from sleep
    //device->UpdateSize = device->Frequency / 50;

    // Register callback functions
    s3eSoundChannelRegister( data->channel, S3E_CHANNEL_GEN_AUDIO, s3e_more_audio, device );

    // Check if we have stereo sound
    if( s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) )
    {
        data->isStereo = s3eSoundChannelRegister( data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO, s3e_more_audio, device ) == S3E_RESULT_SUCCESS;
    } else data->isStereo = 0;
    device->FmtChans = data->isStereo ? DevFmtStereo : DevFmtMono;

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
    //assert( device->UpdateSize == device->Frequency / 50 );

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
    data->preBufferSize = data->sampleSize * 3;
    data->preBufferData = (ALubyte*)calloc( 1, data->preBufferSize * data->bytesPerSample );
    data->preBuffer_DataStartIndex = data->preBuffer_DataEndIndex = 0;

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
    s3eDeviceYield(20); // Give 20ms to thread to mix first data

    // Starting infinite playback cycle with any data
    s3eSoundChannelPlay( data->channel, (int16*)data->mix_data, data->sampleSize * data->bytesPerSample / 2, 0, 0 );
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

        // Notify thread that something has changed
        if( data->thread_semaphore ) s3eThreadSemPost(data->thread_semaphore);

        // Wait for thread to exit gracefully, but don't block indefinitely
        for( i=0; i<20 && !data->thread_exited; i++ ) Sleep(2);
        // And now really stop the thread
        StopThread( thread );
    }
    data->preBuffer_DataStartIndex = data->preBuffer_DataEndIndex = 0;

    // Destroy the semaphore
    if(data->thread_semaphore != NULL) s3eThreadSemDestroy(data->thread_semaphore);

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

