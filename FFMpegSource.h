#pragma once

#include <boost/thread.hpp>

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

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AUDIO_DIFF_AVG_NB 20
#define VIDEO_PICTURE_QUEUE_SIZE 1

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_MASTER,

    AV_SYNC_DEFAULT = AV_SYNC_EXTERNAL_MASTER
};

struct VideoState;

struct PacketQueue {
    PacketQueue()
      : first_pkt(NULL), last_pkt(NULL), flushing(false), nb_packets(0), size(0)
    { }
    ~PacketQueue()
    { clear(); }

    AVPacketList *first_pkt, *last_pkt;
    volatile bool flushing;
    int nb_packets;
    int size;

    boost::mutex mutex;
    boost::condition_variable cond;

    void put(AVPacket *pkt);
    int get(AVPacket *pkt, VideoState *is);

    void flush();
    void clear();
};

struct VideoPicture {
    VideoPicture() : pts(0.0)
    { }

    std::vector<uint8_t> data;
    double pts;
};

struct VideoState {

    VideoState()
      : format_ctx(NULL), av_sync_type(AV_SYNC_DEFAULT)
      , external_clock_base(0.0)
      , audio_st(NULL)
      , video_st(NULL), frame_last_pts(0.0), frame_last_delay(0.0),
        video_clock(0.0), sws_context(NULL), rgbaFrame(NULL), pictq_size(0),
        pictq_rindex(0), pictq_windex(0)
      , refresh_rate_ms(10), refresh(false), quit(false), display_ready(false)
    {
        // Register all formats and codecs
        av_register_all();
    }

    ~VideoState()
    { deinit(); }

    int init(String filename);
    void deinit();

    int stream_open(int stream_index, AVFormatContext *pFormatCtx);

    static void audio_thread_loop(VideoState *is);
    static void video_thread_loop(VideoState *is);
    static void decode_thread_loop(VideoState *is);

    bool update(int screen_width, int screen_height);

    void video_display();
    void video_refresh_timer();

    int queue_picture(AVFrame *pFrame, double pts);
    double synchronize_video(AVFrame *src_frame, double pts);

    static void video_refresh(VideoState *is);

    /* TODO: template here get_audio_clock */
    double get_audio_clock()
    { return NULL; }

    double get_video_clock()
    { return this->frame_last_pts; }

    double get_external_clock()
    { return ((uint64_t)av_gettime()-this->external_clock_base) / 1000000.0; }

    double get_master_clock()
    {
        if(this->av_sync_type == AV_SYNC_VIDEO_MASTER)
            return this->get_video_clock();
        if(this->av_sync_type == AV_SYNC_AUDIO_MASTER)
            return this->get_audio_clock();
        return this->get_external_clock();
    }

    AVFormatContext* format_ctx;

    int av_sync_type;
    uint64_t external_clock_base;

    AVStream**  audio_st;
    PacketQueue audioq;

    AVStream**  video_st;
    double      frame_last_pts;
    double      frame_last_delay;
    double      audio_clock;
    double      video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    PacketQueue videoq;
    SwsContext*  sws_context;
    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    AVFrame*     rgbaFrame; // used as buffer for the frame converted from its native format to RGBA
    int          pictq_size, pictq_rindex, pictq_windex;
    boost::mutex pictq_mutex;
    boost::condition_variable pictq_cond;


    boost::thread parse_thread;
    boost::thread video_thread;
    boost::thread audio_thread;

    boost::thread refresh_thread;
    volatile int refresh_rate_ms;

    volatile bool refresh;
    volatile bool quit;
    volatile bool display_ready;

    Texture *texture;
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
    bool                bRandom;
    bool                bRepeat;
    UINT                renderCX, renderCY;

    FFMpegAudioSource  *audioOut;

    XElement           *data;
    StringList          filesList;
    int                 filesListIndex;

    int                 dedicatedrate;
    AVSampleFormat      dedicatedformat;
    int                 dedicatedlayout;

    struct VideoState  *vs;

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

    Vect2 GetSize() const {return Vect2(float(renderCX), float(renderCY));}
};

