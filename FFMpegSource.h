#pragma once

#define __STDC_CONSTANT_MACROS
extern "C" 
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/time.h>
    #include <libavutil/parseutils.h>
    #include <libavutil/opt.h>
    #include <libswresample/swresample.h>
	#include <libswscale/swscale.h>
}

struct FFMpegSourceInfo
{
	bool *pCapturing;
	XElement **data;
};

class FFMpegSource; 

class FFMpegAudioSource : public AudioSource
{
    FFMpegSource *device;

    UINT sampleSegmentSize, sampleFrameCount;

    HANDLE hAudioMutex;
    List<BYTE> sampleBuffer;
    List<BYTE> outputBuffer;

protected:
    virtual bool GetNextBuffer(void **buffer, UINT *numFrames, QWORD *timestamp);
    virtual void ReleaseBuffer();

    virtual CTSTR GetDeviceName() const;

public:
    bool Initialize(FFMpegSource *parent);
    ~FFMpegAudioSource();

    void writeSample(uint8_t *data, int length);
};

class FFMpegSource : public ImageSource
{
    bool				bFlipVertical;
    UINT				renderCX, renderCY;

	FFMpegAudioSource  *audioOut;

	XElement		   *data;
	Texture			   *texture;
	bool				bCapturing;
	bool				bReadyToDraw;
	Texture			   *noSignalTex;
	unsigned long		Buffers;
	FFMpegSourceInfo	sharedInfo;
	HANDLE              hSampleMutex;
    StringList          filesList;

    AVFormatContext    *format_context;

	AVStream           *video_stream;
    AVStream           *audio_stream;

	AVFrame            *iFrame;
	AVFrame            *pFrameRGB;

	uint8_t            *iBuffer;
	uint8_t            *buffer;
   
    struct SwsContext  *sws_context;
	SwrContext         *swr;

	int                 dedicatedrate;
	AVSampleFormat      dedicatedformat;
	int                 dedicatedlayout;

    double              video_clock;
    double              audio_clock;
   
    double              frame_timer;
    double              frame_last_pts;
    double              frame_last_delay;

public:
    bool Init(XElement *data);
	FFMpegSource();
	~FFMpegSource();

    void UpdateSettings();

    void Preprocess();
    void Render(const Vect2 &pos, const Vect2 &size);

    void BeginScene();
    void EndScene();

	void Start();
    void Stop();

	static HWND hConfigWnd;

	int open_file(String filename);

    Vect2 GetSize() const {return Vect2(float(renderCX), float(renderCY));}
};

