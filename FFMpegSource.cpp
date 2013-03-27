#include "FFMpegInput.h"
#include <string.h>

/*
 TODO
 0) more structs
 1) threading through OSCreateThread
 2) compute frame delay, currently obs fps must be matched with video source fps
 3) implement audio/video synchronization with selected mode:
   a) video to audio
   b) audio to video
   c) external
 4) playback controls, settings, seek, etc...
 5) playlist support
*/

HWND FFMpegSource::hConfigWnd = NULL;

bool FFMpegSource::Init(XElement *data)
{
    traceIn(FFMpegSource::Init);

    this->data = data;
    UpdateSettings();

	bCapturing = false;

    hSampleMutex = OSCreateMutex();
    if(!hSampleMutex)
    {
        AppWarning(TEXT("FFMpegInput: could not create sample mutex"));
        return false;
    }

	XFile file;
	if (file.Open(API->GetPluginDataPath() << TEXT("\\FFMpegInput\\nosignal.png"), XFILE_READ, XFILE_OPENEXISTING)) // load user-defined image if available
	{
		file.Close();
		noSignalTex = CreateTextureFromFile(API->GetPluginDataPath() << TEXT("\\FFMpegInput\\nosignal.png"), TRUE);
	}
	if (!noSignalTex)
		noSignalTex = CreateTextureFromFile(TEXT("plugins\\FFMpegInput\\nosignal.png"), TRUE); // fallback to default image

	return true;

    traceOut;
}

FFMpegSource::FFMpegSource()
{
	bFlipVertical = true;
	sharedInfo.pCapturing = &bCapturing;
}

FFMpegSource::~FFMpegSource()
{
    traceIn(FFMpegSource::~FFMpegSource);

//    Stop();

    traceOut;
}

static AVStream* open_stream(AVFormatContext* format_context, int type) {
        assert(format_context != NULL);
       
        int index;
        AVStream* stream = NULL;
        // Find stream index
        for (index = 0; index < format_context->nb_streams; ++index) {
                if (format_context->streams[index]->codec->codec_type == type) {
                        stream = format_context->streams[index];
                        break;
                }
        }
        if (stream == NULL) {
                // Stream index not found
                return NULL;
        }
       
        AVCodecContext* codec_context = stream->codec;
       
        // Find suitable codec
        AVCodec* codec = avcodec_find_decoder(codec_context->codec_id);
        if (codec == NULL) {
                return NULL;
        }
        if (avcodec_open2(codec_context, codec, NULL) < 0) {
                return NULL;
        }

        return stream;
}

int FFMpegSource::open_file(String filename)
{
    OSEnterMutex(hSampleMutex);
	av_register_all();
    OSLeaveMutex(hSampleMutex);

	format_context = avformat_alloc_context();

	AVDictionary *options = NULL;
	av_dict_set(&options, "rtsp_transport", "tcp", 0);

	if (filename.Length()>7 && filename.Left(7).CompareI(TEXT("rtmp://")))
		filename << " live=1";

	int err = avformat_open_input(&format_context, filename.CreateUTF8String(), NULL, &options);
    if (err < 0) {
            AppWarning(TEXT("ffmpeg: Unable to open input file"));
            return -1;
    }

    err = avformat_find_stream_info(format_context, NULL);
    if (err < 0) {
            AppWarning(TEXT("ffmpeg: Unable to find stream info"));
            return -1;
    }
   
    video_stream = open_stream(format_context, AVMEDIA_TYPE_VIDEO);
    if (video_stream == NULL) {
            AppWarning(TEXT("ffmpeg: Could not open video stream"));
            return -1;
    }
    audio_stream = open_stream(format_context, AVMEDIA_TYPE_AUDIO);
    if (audio_stream == NULL) {
            AppWarning(TEXT("ffmpeg: Could not open audio stream"));
    }


	AVCodecContext* video_codec_context;
	video_codec_context = video_stream->codec;

	int64_t timeBase = (int64_t(video_stream->codec->time_base.num) * AV_TIME_BASE) / int64_t(video_stream->codec->time_base.den);

	int i_buffer_size = avpicture_get_size(video_codec_context->pix_fmt,
                video_codec_context->width, video_codec_context->height);

	iBuffer = (uint8_t *)av_malloc(i_buffer_size);
    iFrame = avcodec_alloc_frame();
    avpicture_fill((AVPicture *)iFrame, iBuffer, video_codec_context->pix_fmt, video_codec_context->width, video_codec_context->height);

    sws_context = sws_getCachedContext(NULL,
                                video_codec_context->width, video_codec_context->height,
                                video_codec_context->pix_fmt,
                                video_codec_context->width, video_codec_context->height,
                                PIX_FMT_RGB32, SWS_BICUBIC,
                                NULL, NULL, NULL);

	pFrameRGB = avcodec_alloc_frame();

	int numBytes;
	numBytes = avpicture_get_size(PIX_FMT_RGB32, video_codec_context->width, video_codec_context->height);
	buffer = (uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

	avpicture_fill((AVPicture *) pFrameRGB, buffer, PIX_FMT_RGB32, video_codec_context->width, video_codec_context->height);

	swr = swr_alloc();
	av_opt_set_int(swr, "in_channel_layout",  audio_stream->codec->channel_layout, 0);
	av_opt_set_int(swr, "out_channel_layout", dedicatedlayout, 0);
//	av_opt_set_int(swr, "in_sample_rate",     audio_stream->codec->sample_rate,                0);
//	av_opt_set_int(swr, "out_sample_rate",    dedicatedrate,                0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt",  audio_stream->codec->sample_fmt, 0);
	av_opt_set_sample_fmt(swr, "out_sample_fmt", dedicatedformat,  0);

    swr_init(swr);

	bCapturing = true;

    renderCX = video_stream->codec->width;
    renderCY = video_stream->codec->height;
    
//    frame_timer = (double)av_gettime() / 1000000.0;
   
    return 0;
}

void FFMpegSource::Start()
{
    traceIn(FFMpegSource::Start);

    renderCX = renderCY = 0;

    sws_context = NULL;
	iFrame = NULL;
    iBuffer = NULL;
	pFrameRGB = NULL;
	buffer = NULL;

	dedicatedrate = 44100;
	dedicatedformat = AV_SAMPLE_FMT_S16;
	dedicatedlayout = AV_CH_LAYOUT_STEREO;//audio_stream->codec->channel_layout;

	String filename = filesList[0];

	int err = open_file(filename);
    if (err < 0) {
		//texture = noSignalTex;
    }

	traceOut;
}

void FFMpegSource::Stop()
{
    traceIn(FFMpegSource::Stop);

	swr_free(&swr);
	sws_freeContext(sws_context);

	av_free(iFrame);
	av_free(iBuffer);
	av_free(pFrameRGB);
	av_free(buffer);

    if(video_stream && avcodec_close(video_stream->codec) < 0) {
        AppWarning(TEXT("[ERROR] Can't close video codec"));
    }

    if(audio_stream && avcodec_close(audio_stream->codec) < 0) {
        AppWarning(TEXT("[ERROR] Can't close audio codec"));
    }

	if(format_context)
	{
        avformat_close_input(&format_context);
		av_free(format_context);
	}

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

    if(!bCapturing)
        return;

	AVPacket packet;
    int frameFinished = 0;

//    VideoData *s = ifCtx->priv_data;
//    AVRational framerate = (ifCtx->priv_data)->framerate;

//----------

//----------
	while (!frameFinished)
	{
		if(av_read_frame(format_context, &packet) >= 0) {
			if(packet.stream_index == video_stream->index) {
				avcodec_decode_video2(video_stream->codec, iFrame, &frameFinished, &packet);
				if (frameFinished)
				{
//				   AVRational framerate = (format_context->priv_data)->framerate;
				   AVRational framerate;
				   framerate = video_stream->codec->time_base;

				    double pts = iFrame->pts;
                    if (pts == AV_NOPTS_VALUE) {
						pts = packet.pts;
					}
                    if (pts == AV_NOPTS_VALUE) {
                        pts = 0;
                    }
					pts *= av_q2d(video_stream->time_base);
					int64_t pts2 = av_rescale(iFrame->pkt_dts, 1000000, AV_TIME_BASE);
					int64_t now = av_gettime();
//					av_usleep(pts2 - now);
					double frame_delay = av_q2d(video_stream->codec->time_base);
					frame_delay += iFrame->repeat_pict * (frame_delay * 0.5);
//					av_usleep(frame_delay * 1000000);
/*
*/
/*
*/
				   sws_scale(sws_context, iFrame->data, iFrame->linesize, 0, video_stream->codec->height, pFrameRGB->data, pFrameRGB->linesize);
				   if (texture)
				   {
				   		delete texture;
						texture = NULL;
				   }
				   texture = CreateTexture(video_stream->codec->width,video_stream->codec->height,GS_BGR, pFrameRGB->data[0], false, false);
				   bReadyToDraw = true;
				}
			}
			else if(packet.stream_index == audio_stream->index)
			{
                int len = avcodec_decode_audio4(audio_stream->codec, iFrame, &frameFinished, &packet);
                if (len < 0) {
//                        av_free_packet(&pkt);
                }
				if (frameFinished)
				{
					int data_size = av_samples_get_buffer_size(NULL, audio_stream->codec->channels, iFrame->nb_samples,	audio_stream->codec->sample_fmt, 1);
					int data_size2 = av_samples_get_buffer_size(NULL, audio_stream->codec->channels, iFrame->nb_samples, dedicatedformat, 1);

					// Obtain audio clock
					if (packet.pts != AV_NOPTS_VALUE) {
							audio_clock = av_q2d(audio_stream->time_base) * packet.pts;
					} else {
							audio_clock += (double)data_size / (audio_stream->codec->channels * audio_stream->codec->sample_rate * av_get_bytes_per_sample(audio_stream->codec->sample_fmt));
					}

					const uint8_t **in = (const uint8_t **)(&iFrame->data[0]);
					uint8_t *output = NULL;
					int out_samples = av_rescale_rnd(audio_stream->codec->sample_rate, dedicatedrate, audio_stream->codec->sample_rate, AV_ROUND_UP);
					int out_linesize;
					av_samples_alloc(&output, NULL, 2, out_samples, dedicatedformat, 1);
					out_samples = swr_convert(swr, &output, iFrame->nb_samples, in, iFrame->nb_samples);

					if (audioOut)
						audioOut->writeSample(output, data_size2);

					av_freep(&output);
					frameFinished = 0;
				}
			}
			av_free_packet(&packet);
		}
	}

	traceOut;
}

void FFMpegSource::Render(const Vect2 &pos, const Vect2 &size)
{
    traceIn(FFMpegSource::Render);
//    if (texture && bReadyToDraw)
    if (texture)
	{
		DrawSprite(texture, 0xFFFFFFFF, pos.x, pos.y, pos.x+size.x, pos.y+size.y);
	}
	else if (noSignalTex)
//	else if (noSignalTex && !bCapturing)
		DrawSprite(noSignalTex, 0xFFFFFFFF, pos.x, pos.y, pos.x+size.x, pos.y+size.y);

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
            AppWarning(TEXT("BitmapTransitionSource::UpdateSettings: Empty path"));
            continue;
        }
    }

	audioOut = new FFMpegAudioSource;
    audioOut->Initialize(this);
    API->AddAudioSource(audioOut);
	API->LeaveSceneMutex();

    traceOut;
}


