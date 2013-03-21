#include "config.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include <s3eSound.h>
#include <s3eAudio.h>

static const ALCchar s3eDevice[] = "s3eSound";


typedef struct {
    int channel;

    ALubyte *buffer;
    ALuint size;
    volatile int killNow;
} s3e_data;

int32 s3e_more_audio(void* systemData, void* userData) {
    ALCdevice *pDevice = (ALCdevice*)userData;
    s3eSoundGenAudioInfo* info = (s3eSoundGenAudioInfo*)systemData;
  
    s3e_data* data = (s3e_data*)pDevice->ExtraData;
    if (data->killNow) {
        info->m_EndSample = S3E_TRUE;
        return 0;
    }
  
    aluMixData(pDevice, info->m_Target, info->m_NumSamples);
    return info->m_NumSamples;
}

/*
static ALuint s3eProc(ALvoid *ptr) {
    ALCdevice *Device = (ALCdevice*)ptr;
    ALuint bytesize = BytesFromDevFmt(Device->FmtType);

    s3e_data *data = (s3e_data*)Device->ExtraData;
    ALuint now, start, i, v;
    ALint b1, b2;
    ALuint64 avail, done;
    const ALuint restTime = (ALuint64)Device->UpdateSize * 1000 /
                            Device->Frequency / 2;
    
    done = 0;
    start = timeGetTime();
    while(!data->killNow && Device->Connected) {
        now = timeGetTime();

        avail = (ALuint64)(now-start) * Device->Frequency / 1000;
        if(avail < done) {
            // Timer wrapped. Add the remainder of the cycle to the available
            // count and reset the number of samples done
            avail += (ALuint64)0xFFFFFFFFu*Device->Frequency/1000 - done;
            done = 0;
        }
        if(avail-done < Device->UpdateSize) {
            //Sleep(restTime);
            Sleep((avail-done)*1000 / Device->Frequency / 2);
            continue;
        }
        
        while(avail-done >= Device->UpdateSize) {
            aluMixData(Device, data->buffer, Device->UpdateSize);
                  s3eSoundChannelPlay(data->channel, (int16*)data->buffer, Device->UpdateSize, 1, 0);
            //s3eAudioPlayFromBuffer(data->buffer, Device->UpdateSize, 1);
            done += Device->UpdateSize;
        }
    }

    return 0;
}
*/

static ALCboolean s3e_open_playback(ALCdevice *device, const ALCchar *deviceName) {
    if( !deviceName ) {
        deviceName = s3eDevice;
    } 
    if( strcmp(s3eDevice, deviceName) == 0 ) {
        s3e_data* data = (s3e_data*)malloc(sizeof(s3e_data));
        data->channel = s3eSoundGetFreeChannel();
        data->buffer = NULL;
        data->size = 0;
        data->killNow = 0;
        device->ExtraData = data;
        device->szDeviceName = strdup(deviceName);
        device->FmtType = DevFmtShort;
        if( s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED)) {
          device->FmtChans = DevFmtStereo;
        } else {
          device->FmtChans = DevFmtMono;
        }
        //device->Frequency = s3eSoundChannelGetInt(data->channel, S3E_CHANNEL_RATE);
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

    data->size = device->UpdateSize * FrameSizeFromDevFmt(device->FmtChans,
                                                          device->FmtType);
    data->buffer = (ALubyte*)malloc(data->size * sizeof(ALubyte));
    memset(data->buffer, 0, data->size * sizeof(ALubyte));
    if(!data->buffer)
    {
        AL_PRINT("buffer malloc failed\n");
        return ALC_FALSE;
    }
    SetDefaultWFXChannelOrder(device);
    //StartThread(s3eProc, device);
    s3eSoundChannelRegister(data->channel, S3E_CHANNEL_GEN_AUDIO, s3e_more_audio, device);
    s3eSoundChannelRegister(data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO, s3e_more_audio, device);
    // Starting infinite playback cycle with any data
    s3eSoundChannelPlay(data->channel, (int16*)data->buffer, data->size, 0, 0);
    
    return ALC_TRUE;
}

static void s3e_stop_playback(ALCdevice *device) {
    s3e_data *data = (s3e_data*)device->ExtraData;

    data->killNow = 1;
    s3eSoundChannelUnRegister(data->channel, S3E_CHANNEL_GEN_AUDIO_STEREO);
    s3eSoundChannelUnRegister(data->channel, S3E_CHANNEL_GEN_AUDIO);
    s3eSoundChannelStop(data->channel);
    free(data->buffer);
    data->buffer = NULL;

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

