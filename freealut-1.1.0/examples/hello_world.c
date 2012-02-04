#include <stdlib.h>
#include <stdio.h>
#include <AL/alut.h>
#include <AL/alc.h>
#include "s3eSound.h"
#include "s3eAudio.h"


/*
  This is the 'Hello World' program from the ALUT
  reference manual.

  Link using '-lalut -lopenal -lpthread'.
*/

int g_closed = 0;

int32 test_close(void *systemData, void *userData) {
	g_closed = 1;
}

int
main (int argc, char **argv)
{

	ALuint helloBuffer, helloSource;
	ALuint fileSize;
#define FILE_NAME "s2.snd"
	int n;	
	int16* buffer;
	FILE* file;

  // make sure s3esound is ready (tends to skip a few seconds of audio on startup)
	int channel = s3eSoundGetFreeChannel();
	int rate = s3eSoundChannelGetInt(channel, S3E_CHANNEL_RATE);
	int volume = s3eSoundChannelGetInt(channel, S3E_CHANNEL_VOLUME);
  
	//s3eAudioPlay(FILE_NAME, 1);
	//alutSleep(5);

	s3eSoundChannelRegister(channel, S3E_CHANNEL_STOP_AUDIO, test_close, NULL);
	file = fopen(FILE_NAME, "rb");

	if( file ) {
		fseek(file, 0L, SEEK_END);
		fileSize = ftell(file);
		fseek(file, 0L, SEEK_SET);
		buffer = (int16*)malloc(fileSize);
		n = fread(buffer, sizeof(int16), fileSize / sizeof(int16), file);
		fclose(file);
		//s3eAudioPlayFromBuffer(buffer, n, 0);
		channel = s3eSoundGetFreeChannel();
		s3eSoundChannelSetInt(channel, S3E_CHANNEL_RATE, 8000);
		//s3eSoundChannelSetInt(channel, S3E_CHANNEL_VOLUME, 19);

		s3eSoundChannelPlay(channel, buffer, n, 1, 0);
		while( !g_closed ) {
			alutSleep(1);
		}
		free(buffer);
	}
	s3eSoundChannelSetInt(channel, S3E_CHANNEL_RATE, rate);

	alutInit (&argc, argv);
	helloBuffer = alutCreateBufferHelloWorld ();
	alGenSources (1, &helloSource);
	alSourcei (helloSource, AL_BUFFER, helloBuffer);
	alSourcePlay (helloSource);
	alutSleep (20);
	s3eSoundChannelSetInt(channel, S3E_CHANNEL_VOLUME, volume);
	alutExit ();
	return EXIT_SUCCESS;
}
