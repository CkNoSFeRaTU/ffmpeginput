#include "FFMpegInput.h"
#include <string.h>

/*
 TODO
 1) implement audio
 2) move syncing to decoding stage and implement frame dropping
 3) init stage must be threaded too or workarounded for nonblocking
 4) playback controls, settings, seek, etc...
 5) playlist support
*/

void PacketQueue::put(AVPacket *pkt)
{
    AVPacketList *pkt1;
    pkt1 = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if(!pkt1) throw std::bad_alloc();
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    if(pkt1->pkt.destruct == NULL)
    {
        if(av_dup_packet(&pkt1->pkt) < 0)
        {
            av_free(pkt1);
            throw std::runtime_error("Failed to duplicate packet");
        }
        av_free_packet(pkt);
    }

    this->mutex.lock ();

    if(!last_pkt)
        this->first_pkt = pkt1;
    else
        this->last_pkt->next = pkt1;
    this->last_pkt = pkt1;
    this->nb_packets++;
    this->size += pkt1->pkt.size;
    this->cond.notify_one();

    this->mutex.unlock();
}

int PacketQueue::get(AVPacket *pkt, VideoState *is)
{
    boost::unique_lock<boost::mutex> lock(this->mutex);
    while(!is->quit)
    {
        AVPacketList *pkt1 = this->first_pkt;
        if(pkt1)
        {
            this->first_pkt = pkt1->next;
            if(!this->first_pkt)
                this->last_pkt = NULL;
            this->nb_packets--;
            this->size -= pkt1->pkt.size;

            *pkt = pkt1->pkt;
            av_free(pkt1);

            return 1;
        }

        if(this->flushing)
            break;
        this->cond.wait(lock);
    }

    return -1;
}

void PacketQueue::flush()
{
    this->flushing = true;
    this->cond.notify_one();
}

void PacketQueue::clear()
{
    AVPacketList *pkt, *pkt1;

    this->mutex.lock();
    for(pkt = this->first_pkt; pkt != NULL; pkt = pkt1)
    {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    this->last_pkt = NULL;
    this->first_pkt = NULL;
    this->nb_packets = 0;
    this->size = 0;
    this->mutex.unlock ();
}

void VideoState::video_refresh(VideoState* is)
{
    boost::system_time t = boost::get_system_time();
    while(!is->quit)
    {
        t += boost::posix_time::milliseconds(is->refresh_rate_ms);
        boost::this_thread::sleep(t);
        is->refresh = true;
    }
}


void VideoState::video_display()
{
    VideoPicture *vp = &this->pictq[this->pictq_rindex];

    if((*this->video_st)->codec->width != 0 && (*this->video_st)->codec->height != 0)
    {
       if (texture)
       {
            delete texture;
            texture = NULL;
       }
       texture = CreateTexture((*this->video_st)->codec->width, (*this->video_st)->codec->height,GS_BGR, &vp->data[0], false, false);
       this->display_ready = true;
    }
}

void VideoState::video_refresh_timer()
{
    VideoPicture *vp;
    double delay;

    if(this->pictq_size == 0)
        return;

    vp = &this->pictq[this->pictq_rindex];

    delay = vp->pts - this->frame_last_pts; /* the pts from last time */
    if(delay <= 0 || delay >= 1.0) {
        /* if incorrect delay, use previous one */
        delay = this->frame_last_delay;
    }
    /* save for next time */
    this->frame_last_delay = delay;
    this->frame_last_pts = vp->pts;

    /* FIXME: Syncing should be done in the decoding stage, where frames can be
     * skipped or duplicated as needed. */

    /* update delay to sync to audio if not master source */
    if(this->av_sync_type != AV_SYNC_VIDEO_MASTER)
    {
        double diff = this->get_video_clock() - this->get_master_clock();

        /* Skip or repeat the frame. Take delay into account
         * FFPlay still doesn't "know if this is the best guess." */
        double sync_threshold = delay>AV_SYNC_THRESHOLD?delay:AV_SYNC_THRESHOLD;
        if(diff <= -sync_threshold)
            delay = 0;
        else if(diff >= sync_threshold)
            delay = 2 * delay;
    }

    this->refresh_rate_ms = std::max<int>(1, (int)(delay*1000.0));
    /* show the picture! */
    this->video_display();

    /* update queue for next picture! */
    this->pictq_rindex = (this->pictq_rindex+1) % VIDEO_PICTURE_QUEUE_SIZE;
    this->pictq_mutex.lock();
    this->pictq_size--;
    this->pictq_cond.notify_one();
    this->pictq_mutex.unlock();
}


int VideoState::queue_picture(AVFrame *pFrame, double pts)
{
    VideoPicture *vp;

    /* wait until we have a new pic */
    {
        boost::unique_lock<boost::mutex> lock(this->pictq_mutex);
        while(this->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !this->quit)
            this->pictq_cond.timed_wait(lock, boost::posix_time::milliseconds(1));
    }
    if(this->quit)
        return -1;

    // windex is set to 0 initially
    vp = &this->pictq[this->pictq_windex];

    // Convert the image into RGB32 format
    if(this->sws_context == NULL)
    {
        int w = (*this->video_st)->codec->width;
        int h = (*this->video_st)->codec->height;
        this->sws_context = sws_getContext(w, h, (*this->video_st)->codec->pix_fmt,
                                           w, h, PIX_FMT_RGB32, SWS_BICUBIC,
                                           NULL, NULL, NULL);
        if(this->sws_context == NULL)
            throw std::runtime_error("Cannot initialize the conversion context!\n");
    }

    vp->pts = pts;
    vp->data.resize((*this->video_st)->codec->width * (*this->video_st)->codec->height * 4);

    uint8_t *dst = &vp->data[0];
    sws_scale(this->sws_context, pFrame->data, pFrame->linesize,
              0, (*this->video_st)->codec->height, &dst, this->rgbaFrame->linesize);

    // now we inform our display thread that we have a pic ready
    this->pictq_windex = (this->pictq_windex+1) % VIDEO_PICTURE_QUEUE_SIZE;
    this->pictq_mutex.lock();
    this->pictq_size++;
    this->pictq_mutex.unlock();

    return 0;
}

double VideoState::synchronize_video(AVFrame *src_frame, double pts)
{
    double frame_delay;

    /* if we have pts, set video clock to it */
    if(pts != 0)
        this->video_clock = pts;
    else
        pts = this->video_clock;

    /* update the video clock */
    frame_delay = av_q2d((*this->video_st)->codec->time_base);

    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    this->video_clock += frame_delay;

    return pts;
}


/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
static uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;
static int our_get_buffer(struct AVCodecContext *c, AVFrame *pic)
{
    int ret = avcodec_default_get_buffer(c, pic);
    uint64_t *pts = (uint64_t*)av_malloc(sizeof(uint64_t));
    *pts = global_video_pkt_pts;
    pic->opaque = pts;
    return ret;
}
static void our_release_buffer(struct AVCodecContext *c, AVFrame *pic)
{
    if(pic) av_freep(&pic->opaque);
    avcodec_default_release_buffer(c, pic);
}

void VideoState::audio_thread_loop(VideoState *self)
{
    /* NOT IMPLEMENTED */
}

void VideoState::video_thread_loop(VideoState *self)
{
    AVPacket pkt1, *packet = &pkt1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;

    pFrame = avcodec_alloc_frame();

    self->rgbaFrame = avcodec_alloc_frame();
    avpicture_alloc((AVPicture*)self->rgbaFrame, PIX_FMT_RGBA, (*self->video_st)->codec->width, (*self->video_st)->codec->height);

    while(self->videoq.get(packet, self) >= 0)
    {
        // Save global pts to be stored in pFrame
        global_video_pkt_pts = packet->pts;
        // Decode video frame
        if(avcodec_decode_video2((*self->video_st)->codec, pFrame, &frameFinished, packet) < 0)
            throw std::runtime_error("Error decoding video frame");

        pts = 0;
        if((uint64_t)packet->dts != AV_NOPTS_VALUE)
            pts = packet->dts;
        else if(pFrame->opaque && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE)
            pts = *(uint64_t*)pFrame->opaque;
        pts *= av_q2d((*self->video_st)->time_base);

        av_free_packet(packet);

        // Did we get a video frame?
        if(frameFinished)
        {
            pts = self->synchronize_video(pFrame, pts);
            if(self->queue_picture(pFrame, pts) < 0)
                break;
        }
    }

    av_free(pFrame);

    avpicture_free((AVPicture*)self->rgbaFrame);
    av_free(self->rgbaFrame);
}

void VideoState::decode_thread_loop(VideoState *self)
{
    AVFormatContext *pFormatCtx = self->format_ctx;
    AVPacket pkt1, *packet = &pkt1;

    try
    {
        if(!self->video_st && !self->audio_st)
            throw std::runtime_error("No streams to decode");

        // main decode loop
        while(!self->quit)
        {
            if((self->audio_st >= 0 && self->audioq.size > MAX_AUDIOQ_SIZE) ||
               (self->video_st >= 0 && self->videoq.size > MAX_VIDEOQ_SIZE))
            {
                boost::this_thread::sleep(boost::posix_time::milliseconds(10));
                continue;
            }

            if(av_read_frame(pFormatCtx, packet) < 0)
                break;

            // Is this a packet from the video stream?
            if(self->video_st && packet->stream_index == self->video_st-pFormatCtx->streams)
                self->videoq.put(packet);
            else if(self->audio_st && packet->stream_index == self->audio_st-pFormatCtx->streams)
                self->audioq.put(packet);
            else
                av_free_packet(packet);
        }

        /* all done - wait for it */
        self->videoq.flush();
        self->audioq.flush();
        while(!self->quit)
        {
            // EOF reached, all packets processed, we can exit now
            if(self->audioq.nb_packets == 0 && self->videoq.nb_packets == 0)
                break;
            boost::this_thread::sleep(boost::posix_time::milliseconds(100));
        }
    }
    catch(std::runtime_error& e) {
        String ErrorMessage = TEXT("An error occured playing the video: ");
        ErrorMessage << e.what();
        AppWarning(ErrorMessage);
    }

    self->quit = true;
}

int VideoState::stream_open(int stream_index, AVFormatContext *pFormatCtx)
{
    AVCodecContext *codecCtx;
    AVCodec *codec;

    if(stream_index < 0 || stream_index >= static_cast<int>(pFormatCtx->nb_streams))
        return -1;

    // Get a pointer to the codec context for the video stream
    codecCtx = pFormatCtx->streams[stream_index]->codec;
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if(!codec || (avcodec_open2(codecCtx, codec, NULL) < 0))
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(codecCtx->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        this->audio_st = pFormatCtx->streams + stream_index;
/* TODO: Currently audio disabled because layer not implemented
        this->audio_thread = boost::thread(audio_thread_loop, this);
*/
        avcodec_close((*this->audio_st)->codec);
        this->audio_st = NULL;
        return -1;
        break;

    case AVMEDIA_TYPE_VIDEO:
        this->video_st = pFormatCtx->streams + stream_index;

        this->frame_last_delay = 40e-3;

        codecCtx->get_buffer = our_get_buffer;
        codecCtx->release_buffer = our_release_buffer;

        this->video_thread = boost::thread(video_thread_loop, this);
        this->refresh_thread = boost::thread(video_refresh, this);
        break;

    default:
        break;
    }

    return 0;
}

int VideoState::init(String filename)
{
    int video_index = -1;
    int audio_index = -1;
    unsigned int i;

    AVDictionary *options = NULL;
    AVInputFormat* avInfo = NULL;

    if (filename.Length()>7 && filename.Left(7).CompareI(TEXT("rtsp://")))
        av_dict_set(&options, "rtsp_transport", "tcp", 0);

    if (filename.Length()>7 && filename.Left(7).CompareI(TEXT("rtmp://")))
        filename << " live=1";

    if (filename.Length()>8 && filename.Left(8).CompareI(TEXT("mjpeg://")))
    {
        filename.FindReplace(TEXT("mjpeg://"),TEXT("http://"));
        avInfo = av_find_input_format("mjpeg");
    }

    this->av_sync_type = AV_SYNC_VIDEO_MASTER;//AV_SYNC_DEFAULT;
    this->refresh_rate_ms = 10;
    this->refresh = false;
    this->quit = false;

    this->format_ctx = avformat_alloc_context();

    // Open video file
    if(!this->format_ctx || avformat_open_input(&this->format_ctx, filename.CreateUTF8String(), avInfo, &options) < 0)
    {
        // "Note that a user-supplied AVFormatContext will be freed on failure."
        this->format_ctx = NULL;
        AppWarning(TEXT("ffmpeg: Unable to open input file"));
        return 0;
    }

    // Retrieve stream information
    if(avformat_find_stream_info(this->format_ctx, NULL) < 0)
    {
        AppWarning(TEXT("ffmpeg: Failed to retrieve stream information"));
        return 0;
    }

    for(i = 0;i < this->format_ctx->nb_streams;i++)
    {
        if(this->format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
            video_index = i;
        if(this->format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
            audio_index = i;
    }

    this->external_clock_base = av_gettime();
    if(audio_index >= 0)
        this->stream_open(audio_index, this->format_ctx);
    if(video_index >= 0)
        this->stream_open(video_index, this->format_ctx);

    this->parse_thread = boost::thread(decode_thread_loop, this);
    return 1;
}

void VideoState::deinit()
{
    this->quit = true;

    this->audioq.cond.notify_one();
    this->videoq.cond.notify_one();

    this->parse_thread.join();
//    this->audio_thread.join();
    this->video_thread.join();
    this->refresh_thread.join();

    if(this->audio_st)
        avcodec_close((*this->audio_st)->codec);
    this->audio_st = NULL;
    if(this->video_st)
        avcodec_close((*this->video_st)->codec);
    this->video_st = NULL;

    if(this->sws_context)
        sws_freeContext(this->sws_context);
    this->sws_context = NULL;

    if(this->format_ctx)
        avformat_close_input(&this->format_ctx);
    av_free(this->format_ctx);
}

HWND FFMpegSource::hConfigWnd = NULL;

bool FFMpegSource::Init(XElement *data)
{
    traceIn(FFMpegSource::Init);

    this->data = data;
    UpdateSettings();

    return true;

    traceOut;
}

FFMpegSource::FFMpegSource()
{
}

FFMpegSource::~FFMpegSource()
{
    traceIn(FFMpegSource::~FFMpegSource);

    traceOut;
}

void FFMpegSource::Start()
{
    traceIn(FFMpegSource::Start);

    renderCX = renderCY = 0;

    dedicatedrate = 44100;
    dedicatedformat = AV_SAMPLE_FMT_S16;
    dedicatedlayout = AV_CH_LAYOUT_STEREO;

    vs = new VideoState;
    int i = 0;
    filesListIndex = -1;
    for (i=0;i<filesList.Num();i++)
    {
        if (vs->init(filesList[i]))
        {
            filesListIndex = i;
            break;
        }
        else
            vs->deinit();
    }

    traceOut;
}

void FFMpegSource::Stop()
{
    traceIn(FFMpegSource::Stop);

    vs->deinit();

    traceOut;
}

void FFMpegSource::BeginScene()
{
    traceIn(FFMpegSource::BeginScene);

    Start();

    traceOut;
}

void FFMpegSource::EndScene()
{
    traceIn(FFMpegSource::EndScene);

    Stop();

    traceOut;
}

void FFMpegSource::Preprocess()
{
    traceIn(FFMpegSource::Preprocess);

    if (vs->quit & filesListIndex>=0)
    {
        if (filesList.Num()>filesListIndex + 1)
            filesListIndex++;
        else if (bRepeat && filesList.Num()>0)
            filesListIndex = 0;
        else
            filesListIndex = -1;

        if (filesListIndex>=0)
        {
            int i;
            vs->deinit();
            vs = new VideoState;
            for (i=filesListIndex;i<filesList.Num();i++)
            {
                if (vs->init(filesList[i]))
                {
                    filesListIndex = i;
                    break;
                }
                else
                    vs->deinit();
            }
        }
    }

    traceOut;
}

void FFMpegSource::Render(const Vect2 &pos, const Vect2 &size)
{
    traceIn(FFMpegSource::Render);

    if(vs->quit)
        return;

    if(vs->refresh)
    {
        vs->refresh = false;
        vs->video_refresh_timer();

        // Correct aspect ratio by adding black bars
        renderCX = (*vs->video_st)->codec->width;
        renderCY = (*vs->video_st)->codec->height;

        double videoaspect = av_q2d((*vs->video_st)->codec->sample_aspect_ratio);
        if(videoaspect == 0.0)
            videoaspect = 1.0;
        videoaspect *= static_cast<double>((*vs->video_st)->codec->width) / (*vs->video_st)->codec->height;

        double screenaspect = static_cast<double>(renderCX) / renderCY;
        double aspect_correction = videoaspect / screenaspect;

        /* TODO aspect correction currently not used */
    }

    if (vs->display_ready && vs->texture)
    {
        DrawSprite(vs->texture, 0xFFFFFFFF, pos.x, pos.y, pos.x+size.x, pos.y+size.y);
    }

    traceOut;
}

void FFMpegSource::UpdateSettings()
{
    traceIn(FFMpegSource::UpdateSettings);

    API->EnterSceneMutex();

    data->GetStringList(TEXT("files"), filesList);
    for(UINT i=0; i<filesList.Num(); i++)
    {
        String &strFile = filesList[i];
        if(strFile.IsEmpty())
        {
            AppWarning(TEXT("ffmpeg::UpdateSettings: Empty path"));
            continue;
        }
    }
    bRandom = data->GetInt(TEXT("random"));
    bRepeat = data->GetInt(TEXT("repeat"));

/*
    audioOut = new FFMpegAudioSource;
    audioOut->Initialize(this);
    API->AddAudioSource(audioOut);
*/
    API->LeaveSceneMutex();

    traceOut;
}
