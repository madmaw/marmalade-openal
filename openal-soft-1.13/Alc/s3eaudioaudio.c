/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include <s3eAudio.h>
#include <s3eSound.h>


typedef struct {
    ALbyte* data;
	long dataLength;
	long availableDataLength;
    long DataStart;

    ALvoid *buffer;
    ALuint size;

    volatile int killNow;
    ALvoid *thread;

	ALboolean audioReady;
} s3eAudio_data;


static const ALCchar s3eAudioDevice[] = "s3e Audio Writer";

static const int HEADER_SIZE = 1024;

static const ALubyte SUBTYPE_PCM[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};
static const ALubyte SUBTYPE_FLOAT[] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};

static const ALuint channel_masks[] = {
    0, /* invalid */
    0x4, /* Mono */
    0x1 | 0x2, /* Stereo */
    0, /* 3 channel */
    0x1 | 0x2 | 0x10 | 0x20, /* Quad */
    0, /* 5 channel */
    0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20, /* 5.1 */
    0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400, /* 6.1 */
    0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400, /* 7.1 */
};


static int write16le(ALushort val, ALbyte* data, int pos)
{
    data[pos++] = (val&0xff);
    data[pos++] = (val>>8)&0xff;
	return pos;
}

static int write32le(ALuint val, ALbyte* data, int pos)
{
    data[pos++] = (val&0xff);
    data[pos++] = ((val>>8)&0xff);
    data[pos++] = ((val>>16)&0xff);
    data[pos++] = ((val>>24)&0xff);
	return pos;
}

static int32 s3eAudioStopped(void *systemData, void *userData) {
	((s3eAudio_data*)userData)->audioReady = 1;
	return 0;
}


static ALuint s3eAudioProc(ALvoid *ptr)
{
    ALCdevice *pDevice = (ALCdevice*)ptr;
    s3eAudio_data *data = (s3eAudio_data*)pDevice->ExtraData;
    ALuint frameSize;
    ALuint now, start;
    ALuint64 avail, done;
    size_t fs;
	int count = 0;
	
    union {
        short s;
        char b[sizeof(short)];
    } uSB;
    const ALuint restTime = (ALuint64)pDevice->UpdateSize * 1000 /
                            pDevice->Frequency / 2;

    uSB.s = 1;
    frameSize = FrameSizeFromDevFmt(pDevice->FmtChans, pDevice->FmtType);

    done = 0;
    start = timeGetTime();
    while(!data->killNow && pDevice->Connected)
    {
		ALbyte* all = data->data;
		long dataLength = data->dataLength;

        now = timeGetTime();

        avail = (ALuint64)(now-start) * pDevice->Frequency / 1000;
        if(avail < done)
        {
            /* Timer wrapped. Add the remainder of the cycle to the available
             * count and reset the number of samples done */
            avail += (ALuint64)0xFFFFFFFFu*pDevice->Frequency/1000 - done;
            done = 0;
        }
        if(avail-done < pDevice->UpdateSize)
        {
            Sleep(restTime);
            continue;
        }
		while( 1 ) {
			
			s3eAudioStatus status = (s3eAudioStatus)s3eAudioGetInt(S3E_AUDIO_STATUS);
			if( S3E_AUDIO_STOPPED == status || S3E_AUDIO_FAILED == status ) {
				break;
			}

			Sleep(100);
		}
		/*
		while(!data->audioReady) {
			// would be nice to synchronize
			Sleep(10);
		}
		*/
		dataLength = data->DataStart;

        while(avail-done >= pDevice->UpdateSize)
        {
			int size;
            aluMixData(pDevice, data->buffer, pDevice->UpdateSize);
            done += pDevice->UpdateSize;

			size = pDevice->UpdateSize * frameSize;
			if( dataLength + size > data->availableDataLength ) {
				// resize the memory
				int reallocSize = pDevice->UpdateSize * 10;
				all = (ALbyte*)realloc(all, data->availableDataLength + reallocSize);
				data->availableDataLength += reallocSize;
				data->data = all;
			}
			if(uSB.b[0] != 1)
            {
                ALuint bytesize = BytesFromDevFmt(pDevice->FmtType);
                ALubyte *bytes = data->buffer;
                ALuint i;

                if(bytesize == 1)
                {
                    for(i = 0;i < data->size;i++)
                        all[dataLength++] = (bytes[i]);
                }
                else if(bytesize == 2)
                {
                    for(i = 0;i < data->size;i++)
                        all[dataLength++] = bytes[i^1];
                }
                else if(bytesize == 4)
                {
                    for(i = 0;i < data->size;i++)
                        all[dataLength++] = (bytes[i^3]);
                }
            }
            else
			{
				memcpy(&all[dataLength], data->buffer, size);
				dataLength += size;
			}
			data->dataLength = dataLength;
        }
		// play whatever we've buffered up
		data->audioReady = 0;
		if(dataLength > 0)
		{
			int adjustedDataLen = dataLength - data->DataStart;
			write32le(adjustedDataLen, data->data, data->DataStart-4); // 'data' header len
			write32le(dataLength-8, data->data, 4); // 'WAVE' header len
		}
		{
			char s[128];
			FILE* f;
			s3eResult r;
			count++;
			sprintf(s, "s%d.wav", count);
			f = fopen(s, "wb");
			fwrite(data->data, data->dataLength, 1, f);
			fclose(f);
			//r = s3eAudioPlay(s, 1);
			r = s3eAudioPlayFromBuffer(data->data, data->dataLength, 1);
			if( r == S3E_RESULT_ERROR ) {
				s3eAudioError err = s3eAudioGetError();
				err = err;
                AL_PRINT("Error playing sound\n");
                //aluHandleDisconnect(pDevice);
                //break;
			}
			//s3eSoundChannelPlay(1, (int16*)(&(data->data[data->DataStart])), (dataLength - data->DataStart)/2, 1, 0);
		}
		//s3eAudioPlayFromBuffer(data->data, data->dataLength, 1); 
    }

    return 0;
}

static ALCboolean s3eAudio_open_playback(ALCdevice *device, const ALCchar *deviceName)
{
    s3eAudio_data *data;
    const char *fname;

    if(!deviceName)
		deviceName = s3eAudioDevice;
    else if(strcmp(deviceName, s3eAudioDevice) != 0)
        return ALC_FALSE;

    data = (s3eAudio_data*)calloc(1, sizeof(s3eAudio_data));

	device->szDeviceName = strdup(deviceName);
    device->ExtraData = data;
	//s3eAudioRegister(S3E_AUDIO_STOP, s3eAudioStopped, data);
    return ALC_TRUE;
}

static void s3eAudio_close_playback(ALCdevice *device)
{
    s3eAudio_data *data = (s3eAudio_data*)device->ExtraData;
	//s3eAudioUnRegister(S3E_AUDIO_STOP, s3eAudioStopped);

    free(data);
    device->ExtraData = NULL;
}

static ALCboolean s3eAudio_reset_playback(ALCdevice *device)
{
    s3eAudio_data *data = (s3eAudio_data*)device->ExtraData;
    ALuint channels=0, bits=0;
	const ALubyte* subtype;
	int i;
	int pos = 0;
	ALbyte* all;

    switch(device->FmtType)
    {
        case DevFmtByte:
            device->FmtType = DevFmtUByte;
            break;
        case DevFmtUShort:
            device->FmtType = DevFmtShort;
            break;
        case DevFmtUByte:
        case DevFmtShort:
        case DevFmtFloat:
            break;
    }
    bits = BytesFromDevFmt(device->FmtType) * 8;
    channels = ChannelsFromDevFmt(device->FmtChans);

	data->availableDataLength = HEADER_SIZE;
	all = (ALbyte*)malloc(sizeof(ALbyte) * HEADER_SIZE);

	all[pos++] = 'R';
	all[pos++] = 'I';
	all[pos++] = 'F';
	all[pos++] = 'F';
    pos = write32le(0xFFFFFFFF, all, pos); // 'RIFF' header len; filled in at close

	all[pos++] = 'W';
	all[pos++] = 'A';
	all[pos++] = 'V';
	all[pos++] = 'E';

	all[pos++] = 'f';
	all[pos++] = 'm';
	all[pos++] = 't';
	all[pos++] = ' ';

	pos = write32le(40, all, pos); // 'fmt ' header len; 40 bytes for EXTENSIBLE

    // 16-bit val, format type id (extensible: 0xFFFE)
    //pos = write16le(0xFFFE, all, pos);
	pos = write16le(0x0001, all, pos);
    // 16-bit val, channel count
    pos = write16le(channels, all, pos);
    // 32-bit val, frequency
    pos = write32le(device->Frequency, all, pos);
    // 32-bit val, bytes per second
    pos = write32le(device->Frequency * channels * bits / 8, all, pos);
    // 16-bit val, frame size
    pos = write16le(channels * bits / 8, all, pos);
    // 16-bit val, bits per sample
    pos = write16le(bits, all, pos);
    // 16-bit val, extra byte count
    pos = write16le(22, all, pos);
    // 16-bit val, valid bits per sample
    pos = write16le(bits, all, pos);
    // 32-bit val, channel mask
    pos = write32le(channel_masks[channels], all, pos);
    // 16 byte GUID, sub-type format
	if( bits == 32 ) {
		subtype = SUBTYPE_FLOAT;
	} else {
		subtype = SUBTYPE_PCM;
	}
	for( i=0; i<16; i++ ) {
		all[pos++] = subtype[i];
	}

	all[pos++] = 'd';
	all[pos++] = 'a';
	all[pos++] = 't';
	all[pos++] = 'a';

	pos = write32le(0xFFFFFFFF, all, pos); // 'data' header len; filled in at close

    data->DataStart = pos;
	data->dataLength = pos;
	data->data = all;

    data->size = device->UpdateSize * channels * bits / 8;
    data->buffer = malloc(data->size);
    if(!data->buffer)
    {
        AL_PRINT("buffer malloc failed\n");
        return ALC_FALSE;
    }

    SetDefaultWFXChannelOrder(device);

	data->audioReady = 1;

    data->thread = StartThread(s3eAudioProc, device);
    if(data->thread == NULL)
    {
        free(data->buffer);
        data->buffer = NULL;
		free(data->data);
		data->data = NULL;
        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static void s3eAudio_stop_playback(ALCdevice *device)
{
    s3eAudio_data *data = (s3eAudio_data*)device->ExtraData;
    ALuint dataLen;
    long size;

    if(!data->thread)
        return;

    data->killNow = 1;
    StopThread(data->thread);
    data->thread = NULL;

    data->killNow = 0;

    free(data->buffer);
    data->buffer = NULL;

	size = data->dataLength;
    if(size > 0)
    {
        dataLen = size - data->DataStart;
        write32le(dataLen, data->data, data->DataStart-4); // 'data' header len
        write32le(size-8, data->data, 4); // 'WAVE' header len
    }
	// and now we play it!
	//s3eAudioPlayFromBuffer(data->data, data->dataLength, 1);
	// TODO clean up the memory
	
}


static ALCboolean s3eAudio_open_capture(ALCdevice *pDevice, const ALCchar *deviceName)
{
    (void)pDevice;
    (void)deviceName;
    return ALC_FALSE;
}


BackendFuncs s3eAudio_funcs = {
    s3eAudio_open_playback,
    s3eAudio_close_playback,
    s3eAudio_reset_playback,
    s3eAudio_stop_playback,
    s3eAudio_open_capture,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

void alc_s3eAudio_init(BackendFuncs *func_list)
{
    *func_list = s3eAudio_funcs;
}

void alc_s3eAudio_deinit(void)
{
}

void alc_s3eAudio_probe(int type)
{
    if(type == DEVICE_PROBE)
        AppendDeviceList(s3eAudioDevice);
    else if(type == ALL_DEVICE_PROBE)
        AppendAllDeviceList(s3eAudioDevice);
}
