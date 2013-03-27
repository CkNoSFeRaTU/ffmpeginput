
#include "FFMpegInput.h"

#include <Audioclient.h>


bool FFMpegAudioSource::GetNextBuffer(void **buffer, UINT *numFrames, QWORD *timestamp)
{
    if(sampleBuffer.Num() >= sampleSegmentSize)
    {
        OSEnterMutex(hAudioMutex);

        mcpy(outputBuffer.Array(), sampleBuffer.Array(), sampleSegmentSize);
        sampleBuffer.RemoveRange(0, sampleSegmentSize);

        OSLeaveMutex(hAudioMutex);

        *buffer = outputBuffer.Array();
        *numFrames = sampleFrameCount;
        *timestamp = API->GetAudioTime();//+GetTimeOffset();

        return true;
    }

    return false;
}

void FFMpegAudioSource::ReleaseBuffer()
{
}


CTSTR FFMpegAudioSource::GetDeviceName() const
{
    return NULL;
}

bool FFMpegAudioSource::Initialize(FFMpegSource *parent)
{
    device = parent;
    hAudioMutex = OSCreateMutex();

    //---------------------------------

    bool  bFloat = false;
    UINT  inputChannels;
    UINT  inputSamplesPerSec;
    UINT  inputBitsPerSample;
    UINT  inputBlockSize;
    DWORD inputChannelMask;

    //---------------------------------
	//32/8/n - AV_SAMPLE_FMT_S32
	//16/4/n - AV_SAMPLE_FMT_S16
	//16/8/y - AV_SAMPLE_FMT_FLT

    inputBitsPerSample = 16;//device->audioFormat.wBitsPerSample;
    inputBlockSize     = 4;//device->audioFormat.nBlockAlign;
    inputChannelMask   = 0;
    inputChannels      = 2;//device->audioFormat.nChannels;
    inputSamplesPerSec = 44100;//device->audioFormat.nSamplesPerSec;

    sampleFrameCount   = inputSamplesPerSec/25; //framerate? why was 100?
    sampleSegmentSize  = inputBlockSize*sampleFrameCount;

    outputBuffer.SetSize(sampleSegmentSize);

    InitAudioData(bFloat, inputChannels, inputSamplesPerSec, inputBitsPerSample, inputBlockSize, inputChannelMask);

    return true;
}

FFMpegAudioSource::~FFMpegAudioSource()
{
    if(hAudioMutex)
        OSCloseMutex(hAudioMutex);
}

void FFMpegAudioSource::writeSample(uint8_t *data, int length)
{
    OSEnterMutex(hAudioMutex);
    sampleBuffer.AppendArray(data, length);
    OSLeaveMutex(hAudioMutex);
}
