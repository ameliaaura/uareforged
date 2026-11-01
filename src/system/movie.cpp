extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h> 
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "movie.h"
#include "fsmgr.h"
#include "inivals.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "system.h"
#include "sound.h"

#if (LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(57, 33, 100))
#define OLDCTX 
#endif

#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 12, 100))
#define OLDPKT 
#endif

#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 106, 102))
#define OLDDECODE 
#endif

#if (LIBAVUTIL_VERSION_INT < AV_VERSION_INT(56, 0, 100))
#define OLDPTS 
#endif

#if (LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57, 24, 100))
#define OLDCHANNEL
#endif

namespace System
{

TMovie TMovie::Instance;
    
struct TMovie::Context
{
    AVFormatContext *FormatCtx = NULL;
    AVCodecContext  *VidCodecCtx = NULL;
    AVCodecContext  *AudCodecCtx = NULL;
    
#if LIBAVCODEC_VERSION_MAJOR < 59
    AVCodec         *VidCodec = NULL;
    AVCodec         *AudCodec = NULL;
#else
    const AVCodec   *VidCodec = NULL;
    const AVCodec   *AudCodec = NULL;
#endif
    
    int videoStream = -1;
    int audioStream = -1;
    
    std::list<AVFrame *> vidFrames;
    std::list<AVFrame *> audFrames;
    
    AVPacket *packet = NULL;
    AVFrame *vidFrame = NULL;
    AVFrame *audFrame = NULL;
    
    bool playing = false;
    bool fileEOF = false;

    // Resolved once at PlayMovie so the per-frame path does not re-read the ini.
    MovieScaleMode scaleMode = MOVIE_SCALE_MAX;
    
    struct SwsContext *sws_ctx = NULL;
    GLuint screenTex = 0;
    
    uint8_t *dst_data[4];
    int dst_linesize[4];    
    
    bool dumpedFrameState = false;

    bool AudioSetted = false;
    struct SwrContext *swr_ctx = NULL;
    uint8_t **adst_data = NULL;
    int adst_linesize = 0;
    int adst_nb_samples = 0;
};

TMovie::TMovie()
{
    _ctx = new Context();
}    

TMovie::~TMovie()
{
    delete _ctx;
}

void TMovie::Init()
{
#if (LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100))
    av_register_all();
#endif
}

int TMovie::EventsWatcher(void *, SDL_Event *event)
{
    switch(event->type)
    {
        case SDL_WINDOWEVENT:
        {
            switch(event->window.event)
            {
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                {
                    Common::Point scrSz = System::GetResolution();
                    glViewport(0, 0, scrSz.x, scrSz.y);
                }
                break;
            default:
                break;
            }

        }
        break;
        
        case SDL_KEYDOWN:
            Instance._ctx->playing = false;
            return 0;

        case SDL_KEYUP:
        case SDL_TEXTINPUT:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
            return 0;
    }
    
    return 1; // This event can be passed to another event watcher
}


namespace {

// A movie reference ("mov:intro.mpg") goes through the FSMgr node tree; an
// hd-fmv replacement is already a real path off the game CWD. Returns empty on
// a reference that resolves to nothing.
std::string MovieDiskPath(const std::string &fname, bool directPath)
{
    if (directPath)
        return fname;

    FSMgr::iNode *file = FSMgr::iDir::findNode(fname);

    return file ? file->getPath() : std::string();
}

}

// ---------------------------------------------------------------------------
// Diagnostic (inert unless UA_GLDUMP=1): print the GL state the movie player
// hands back to the renderer, plus the engine's own cached GfxStates, so a
// before/after diff around a movie can be taken unattended.
// ---------------------------------------------------------------------------
void MovieGLDump(const char *tag)
{
    static int on = -1;
    if ( on < 0 )
    {
        const char *v = getenv("UA_GLDUMP");
        on = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if ( !on )
        return;

    GLint vp[4] = {0,0,0,0};
    GLint tex = 0, prog = 0, minf = 0, magf = 0, sb = 0, db = 0, dfn = 0, dmask = 0, align = 0, fbo = 0;
    GLboolean dtest = 0, blend = 0;

    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(0x8B8D /*GL_CURRENT_PROGRAM*/, &prog);
    glGetIntegerv(GL_BLEND_SRC, &sb);
    glGetIntegerv(GL_BLEND_DST, &db);
    glGetIntegerv(GL_DEPTH_FUNC, &dfn);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &dmask);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbo);
    glGetBooleanv(GL_DEPTH_TEST, &dtest);
    glGetBooleanv(GL_BLEND, &blend);

    if ( tex )
    {
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
    }

    const GFX::GfxStates &st = GFX::Engine.States();
    const GFX::GfxStates &ls = GFX::Engine.GetLastStates();

    printf("[GLDUMP %s] vp=%d,%d,%d,%d fbo=%d bindTex=%d prog=%d min=0x%x mag=0x%x "
           "depth=%d dfunc=0x%x dmask=%d blend=%d src=0x%x dst=0x%x align=%d | "
           "states.Tex=%u states.TexMip=%d last.Tex=%u last.TexMip=%d last.Prog=%u\n",
           tag, vp[0], vp[1], vp[2], vp[3], fbo, tex, prog, minf, magf,
           (int)dtest, dfn, dmask, (int)blend, sb, db, align,
           st.Tex, (int)st.TexMip, ls.Tex, (int)ls.TexMip, ls.Prog.ID);
    fflush(stdout);
}

bool TMovie::OpenFile(const std::string &fname, bool directPath)
{
    std::string diskPath = MovieDiskPath(fname, directPath);

    if (diskPath.empty())
        return false;

    // Trying to open video file
    if (avformat_open_input(&_ctx->FormatCtx, diskPath.c_str(), NULL, NULL) != 0)
        return false;
    
    _ctx->fileEOF = false;
    
    if (avformat_find_stream_info(_ctx->FormatCtx, NULL) < 0)
      return false;
    
    _ctx->videoStream = av_find_best_stream(_ctx->FormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &_ctx->VidCodec, 0);
    
    if (_ctx->videoStream == -1)
        return false;
    
    _ctx->VidCodecCtx = avcodec_alloc_context3(_ctx->VidCodec);
#ifndef OLDCTX
    avcodec_parameters_to_context(_ctx->VidCodecCtx, _ctx->FormatCtx->streams[_ctx->videoStream]->codecpar);
#else
    avcodec_copy_context(_ctx->VidCodecCtx, _ctx->FormatCtx->streams[_ctx->videoStream]->codec);
#endif
    
    if (avcodec_open2(_ctx->VidCodecCtx, _ctx->VidCodec, NULL) < 0)
    {
        avcodec_free_context(&_ctx->VidCodecCtx);
        avformat_close_input(&_ctx->FormatCtx);
        return false;
    }

#ifndef OLDPKT
    _ctx->packet = av_packet_alloc();
#else
    _ctx->packet = (AVPacket *)av_malloc(sizeof(AVPacket));
    av_init_packet(_ctx->packet);
#endif
    _ctx->vidFrame = av_frame_alloc();

    _ctx->audioStream = av_find_best_stream(_ctx->FormatCtx, AVMEDIA_TYPE_AUDIO, -1, _ctx->videoStream, &_ctx->AudCodec, 0);
    _ctx->AudioSetted = false;
    
    _ctx->swr_ctx = NULL;
    _ctx->adst_data = NULL;
    _ctx->adst_linesize = 0;
    _ctx->adst_nb_samples = 0;
    
    if (_ctx->audioStream >= 0)
    {
        _ctx->AudCodecCtx = avcodec_alloc_context3(_ctx->AudCodec);
#ifndef OLDCTX
        avcodec_parameters_to_context(_ctx->AudCodecCtx, _ctx->FormatCtx->streams[_ctx->audioStream]->codecpar);
#else 
        avcodec_copy_context(_ctx->AudCodecCtx, _ctx->FormatCtx->streams[_ctx->audioStream]->codec);
#endif
        
        int ret = avcodec_open2(_ctx->AudCodecCtx, _ctx->AudCodec, NULL);
        if (ret < 0)
        {
            avcodec_free_context(&_ctx->AudCodecCtx);
            _ctx->audioStream = -1;
        }
        
        _ctx->audFrame = av_frame_alloc();
    }
    
    return true;
}

void TMovie::Close()
{
    _ctx->playing = false;
    
#ifndef OLDPKT
    av_packet_free(&_ctx->packet);
#else
    av_free_packet(_ctx->packet);
    av_free(_ctx->packet);
#endif
    av_frame_free(&_ctx->vidFrame);
    
    if (_ctx->audFrame)
        av_frame_free(&_ctx->audFrame);
    
    if (_ctx->sws_ctx)
    {
        sws_freeContext(_ctx->sws_ctx);
        _ctx->sws_ctx = NULL;
        
        av_freep(&_ctx->dst_data[0]);
    }
    
    while(!_ctx->audFrames.empty())
    {
        av_frame_free(&_ctx->audFrames.front());
        _ctx->audFrames.pop_front();
    }
    
    while(!_ctx->vidFrames.empty())
    {
        av_frame_free(&_ctx->vidFrames.front());
        _ctx->vidFrames.pop_front();
    }
    
    avcodec_free_context(&_ctx->AudCodecCtx);
    avcodec_free_context(&_ctx->VidCodecCtx);
    avformat_close_input(&_ctx->FormatCtx);
    
    _ctx->audioStream = -1;
    _ctx->videoStream = -1;
    
    if (_ctx->screenTex != 0)
    {
        GFX::Engine.ForgetTexture(_ctx->screenTex);
        glDeleteTextures(1, &_ctx->screenTex);
    }
    
    if (_ctx->swr_ctx)
    {
        swr_free(&_ctx->swr_ctx);
        _ctx->swr_ctx = NULL;
    }
    
    if (_ctx->adst_data)
    {
        av_freep(&_ctx->adst_data[0]);
        av_freep(_ctx->adst_data);
    }
    
    _ctx->screenTex = 0;
}

void TMovie::ReadFrames()
{
    if ( (_ctx->vidFrames.empty() || _ctx->audFrames.empty()) ) 
    {
        int ret = av_read_frame(_ctx->FormatCtx, _ctx->packet);
        if ( ret >= 0 )
        {
            if (_ctx->packet->stream_index == _ctx->videoStream) 
            {
#ifndef OLDDECODE
                ret = avcodec_send_packet(_ctx->VidCodecCtx, _ctx->packet);
                if (ret < 0)
                {
                    _ctx->playing = false;
                }
                else
                {
                    ret = 0;

                    while (ret >= 0 ) 
                    {
                        ret = avcodec_receive_frame(_ctx->VidCodecCtx, _ctx->vidFrame);

                        if (ret < 0)
                            break;

                        _ctx->vidFrames.push_back( av_frame_clone(_ctx->vidFrame) );
                        av_frame_unref(_ctx->vidFrame);
                    }                    
                }
#else
                int gotFrame = 0;
                ret = avcodec_decode_video2(_ctx->VidCodecCtx, _ctx->vidFrame, &gotFrame, _ctx->packet);
                if (ret < 0)
                {
                    _ctx->playing = false;
                }
                else if (gotFrame)
                {
                    _ctx->vidFrames.push_back( av_frame_clone(_ctx->vidFrame) );
                    av_frame_unref(_ctx->vidFrame);
                }
#endif
            }
            else if (_ctx->audioStream >= 0 && _ctx->packet->stream_index == _ctx->audioStream) 
            {
#ifndef OLDDECODE
                ret = avcodec_send_packet(_ctx->AudCodecCtx, _ctx->packet);
                if (ret < 0)
                {
                    _ctx->playing = false;
                }
                else
                {
                    ret = 0;

                    while (ret >= 0 ) 
                    {
                        ret = avcodec_receive_frame(_ctx->AudCodecCtx, _ctx->audFrame);

                        if (ret < 0)
                            break;
                        
                        _ctx->audFrames.push_back( av_frame_clone(_ctx->audFrame) );
                        av_frame_unref(_ctx->audFrame);
                    }                    
                }
#else
                int gotFrame = 0;
                ret = avcodec_decode_audio4(_ctx->AudCodecCtx, _ctx->audFrame, &gotFrame, _ctx->packet);
                if (ret < 0)
                {
                    _ctx->playing = false;
                }
                else if (gotFrame)
                {
                    _ctx->audFrames.push_back( av_frame_clone(_ctx->audFrame) );
                    av_frame_unref(_ctx->audFrame);
                }
#endif
            }
            
            av_packet_unref(_ctx->packet);
        }
        else
        {
            if (ret == AVERROR_EOF)
                _ctx->fileEOF = true;
            else
                _ctx->playing = false;
        }
    }
}

void TMovie::ProcessFrame(uint32_t tm)
{
    if (_ctx->vidFrames.empty())
        return;
    
    AVFrame *frm = _ctx->vidFrames.front();

    AVStream * vids = _ctx->FormatCtx->streams[_ctx->videoStream];
    
#ifndef OLDPTS // + frm->pkt_duration
    uint32_t frmTime = 1000 * (frm->best_effort_timestamp) * vids->time_base.num / vids->time_base.den;
#else
    uint32_t frmTime = 1000 * av_frame_get_best_effort_timestamp(frm) * vids->time_base.num / vids->time_base.den;
#endif
    
    if (frmTime > tm)
        return;
    
    _ctx->vidFrames.pop_front();

    if (!_ctx->sws_ctx)
    {
        _ctx->sws_ctx = sws_getContext(frm->width,
                        frm->height,
                        _ctx->VidCodecCtx->pix_fmt,
                        frm->width,
                        frm->height,
                        AV_PIX_FMT_RGB24,
                        SWS_BILINEAR,
                        NULL,
                        NULL,
                        NULL
                        );

        av_image_alloc(_ctx->dst_data, _ctx->dst_linesize,
                  frm->width, frm->height, AV_PIX_FMT_RGB24, 1);


    }

    sws_scale(_ctx->sws_ctx, (uint8_t const * const *)frm->data,
            frm->linesize, 0, frm->height,
            _ctx->dst_data, _ctx->dst_linesize);


    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);
    
    GFX::Engine.SetProjectionMatrix( mat4x4f() );
    GFX::Engine.SetModelViewMatrix( mat4x4f() );

    GFX::GfxStates &pStates = GFX::Engine.States();
    GFX::GfxStates saved = pStates;

    pStates.Zwrite = false;
    pStates.DepthTest = false;
    pStates.AlphaBlend = false;
    pStates.Prog = GFX::Engine.GetStdShaderProg();

    GFX::Engine.SetRenderStates(0);
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f );
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (_ctx->screenTex == 0)
    {
        glGenTextures(1, &_ctx->screenTex);
        
        pStates.Tex = _ctx->screenTex;
        // A film frame is a raw GL texture with no mipmap chain. Without this
        // the per-bind filter stamp uses whatever TexMip the last 3D draw left
        // - true for every mipmapped level texture - and GL_LINEAR_MIPMAP_LINEAR
        // on a chainless texture is mipmap-INCOMPLETE, which samples as black.
        // Every other raw-texture site sets it (gfx.cpp DrawScreenSurface,
        // DrawTextLayer, DrawFBO).
        pStates.TexMip = false;
        GFX::Engine.SetRenderStates(0);
        
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frm->width, frm->height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    }
    
    pStates.Tex = _ctx->screenTex;
    pStates.TexMip = false;
    GFX::Engine.SetRenderStates(0);

    if ( !_ctx->dumpedFrameState )
    {
        _ctx->dumpedFrameState = true;
        MovieGLDump("frame-tex-bound");
    }

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frm->width, frm->height, GL_RGB, GL_UNSIGNED_BYTE, _ctx->dst_data[0]);
    
    // Destination rect in window pixels, then the same rect as a fraction of
    // the half-extent of clip space, which is what the quad below wants.
    // Under max the rect fits inside the window and any leftover shows as bars.
    // Under fill it is larger than the window on one axis: the quad simply runs
    // off those edges and the viewport clips it, which is the crop. UVs stay
    // 0..1 in both cases — the frame is sampled whole, and never squashed.
    MovieRect dst = MovieDestRect(_ctx->scaleMode, scrSz.x, scrSz.y, frm->width, frm->height);

    double qW = (double)dst.w / (double)scrSz.x;
    double qH = (double)dst.h / (double)scrSz.y;

    // Not static: the rect changes with the window size (and between movies of
    // different dimensions), so it has to be rebuilt per frame.
    std::array<GFX::TVertex, 4> vtx = {
        GFX::TVertex( vec3f(-1.0 * qW,  1.0 * qH, 0.0), tUtV(0.0, 0.0) ),
        GFX::TVertex( vec3f(-1.0 * qW, -1.0 * qH, 0.0), tUtV(0.0, 1.0) ),
        GFX::TVertex( vec3f( 1.0 * qW, -1.0 * qH, 0.0), tUtV(1.0, 1.0) ),
        GFX::TVertex( vec3f( 1.0 * qW,  1.0 * qH, 0.0), tUtV(1.0, 0.0) ),
    };
    
    GFX::Engine.DrawVtxQuad(vtx);

    System::Flip();
    
    pStates = saved;

    av_frame_free(&frm);
}

void TMovie::ProcessAudio()
{
    size_t minLeft = SFXEngine::SFXe.AudioStream->BuffersCapacity() * 2;
    
    while (!_ctx->audFrames.empty())
    {
        if (SFXEngine::SFXe.AudioStream->DataLeft() >= minLeft)
            break;
        
        AVFrame *frm = _ctx->audFrames.front();
        _ctx->audFrames.pop_front();

        AVStream * s = _ctx->FormatCtx->streams[_ctx->audioStream];
#ifndef OLDPTS
        uint32_t ts = 1000 * (frm->best_effort_timestamp) * s->time_base.num / s->time_base.den;
#else
        uint32_t ts = 1000 * av_frame_get_best_effort_timestamp(frm) * s->time_base.num / s->time_base.den;
#endif
        
#ifndef OLDCHANNEL
        int32_t channels = frm->ch_layout.nb_channels;
#else
        int32_t channels = frm->channels;
#endif

        if (!_ctx->AudioSetted)
        {
            switch(_ctx->AudCodecCtx->sample_fmt)
            {
                case AV_SAMPLE_FMT_U8P:
#ifndef OLDCHANNEL                    
                    swr_alloc_set_opts2(&_ctx->swr_ctx, &frm->ch_layout, AV_SAMPLE_FMT_U8, _ctx->AudCodecCtx->sample_rate, 
                                                &_ctx->AudCodecCtx->ch_layout, _ctx->AudCodecCtx->sample_fmt, _ctx->AudCodecCtx->sample_rate, 
                                          0, NULL);
                    swr_init(_ctx->swr_ctx);
                    av_samples_alloc_array_and_samples(&_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_U8, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
#else
                    _ctx->swr_ctx = swr_alloc_set_opts(NULL, (frm->channels > 1 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO), AV_SAMPLE_FMT_U8, _ctx->AudCodecCtx->sample_rate, 
                                                             _ctx->AudCodecCtx->channel_layout, _ctx->AudCodecCtx->sample_fmt, _ctx->AudCodecCtx->sample_rate, 
                                                       0, NULL);
                    swr_init(_ctx->swr_ctx);
                    av_samples_alloc_array_and_samples(&_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_U8, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
#endif
                case AV_SAMPLE_FMT_U8:
                    if (channels > 1)
                        SFXEngine::SFXe.AudioStream->SetFormat(AL_FORMAT_STEREO8, _ctx->AudCodecCtx->sample_rate);
                    else
                        SFXEngine::SFXe.AudioStream->SetFormat(AL_FORMAT_MONO8, _ctx->AudCodecCtx->sample_rate);
                    break;


                case AV_SAMPLE_FMT_S16P:
                default:
#ifndef OLDCHANNEL 
                    swr_alloc_set_opts2(&_ctx->swr_ctx, &frm->ch_layout, AV_SAMPLE_FMT_S16, _ctx->AudCodecCtx->sample_rate, 
                                              &_ctx->AudCodecCtx->ch_layout, _ctx->AudCodecCtx->sample_fmt, _ctx->AudCodecCtx->sample_rate, 
                                          0, NULL);
                    swr_init(_ctx->swr_ctx);
                    av_samples_alloc_array_and_samples(&_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_S16, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
#else
                    _ctx->swr_ctx = swr_alloc_set_opts(NULL, (frm->channels > 1 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO), AV_SAMPLE_FMT_S16, _ctx->AudCodecCtx->sample_rate, 
                                                             _ctx->AudCodecCtx->channel_layout, _ctx->AudCodecCtx->sample_fmt, _ctx->AudCodecCtx->sample_rate, 
                                                       0, NULL);
                    swr_init(_ctx->swr_ctx);
                    av_samples_alloc_array_and_samples(&_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_S16, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
#endif
                case AV_SAMPLE_FMT_S16:
                    if (channels > 1)
                        SFXEngine::SFXe.AudioStream->SetFormat(AL_FORMAT_STEREO16, _ctx->AudCodecCtx->sample_rate);
                    else
                        SFXEngine::SFXe.AudioStream->SetFormat(AL_FORMAT_MONO16, _ctx->AudCodecCtx->sample_rate);
                    break;
            }

            _ctx->AudioSetted = true;
        }

        switch(_ctx->AudCodecCtx->sample_fmt)
        {
            case AV_SAMPLE_FMT_U8:
                SFXEngine::SFXe.AudioStream->Feed(frm->data[0], frm->nb_samples * channels, ts);
                break;

            case AV_SAMPLE_FMT_S16:
                SFXEngine::SFXe.AudioStream->Feed(frm->data[0], frm->nb_samples * channels * 2, ts);
                break;


            case AV_SAMPLE_FMT_U8P:
                if (frm->nb_samples > _ctx->adst_nb_samples)
                {
                    av_freep(&_ctx->adst_data[0]);
                    av_samples_alloc(_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_U8, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
                }
                swr_convert(_ctx->swr_ctx, _ctx->adst_data, frm->nb_samples, (const uint8_t **)frm->data, frm->nb_samples);

                SFXEngine::SFXe.AudioStream->Feed(_ctx->adst_data[0], frm->nb_samples * Common::MIN(2, channels), ts);
                break;

            case AV_SAMPLE_FMT_S16P:
            default:
                if (frm->nb_samples > _ctx->adst_nb_samples)
                {
                    av_freep(&_ctx->adst_data[0]);
                    av_samples_alloc(_ctx->adst_data, &_ctx->adst_linesize, Common::MIN(2, channels), frm->nb_samples, AV_SAMPLE_FMT_S16, 0);
                    _ctx->adst_nb_samples = frm->nb_samples;
                }
                swr_convert(_ctx->swr_ctx, _ctx->adst_data, frm->nb_samples, (const uint8_t **)frm->data, frm->nb_samples);

                SFXEngine::SFXe.AudioStream->Feed(_ctx->adst_data[0], frm->nb_samples * 2 * Common::MIN(2, channels), ts);
                break;
        }

        av_frame_free(&frm);
    }
}

// ---------------------------------------------------------------------------
// "Modernised Videos" — the hd-fmv redirect.
//
// Every in-game movie (intro, the four faction briefings, black_intro, the
// win/lose extros, the tutorial reels) is resolved from a "mov:NAME.mpg"
// reference and handed to TMovie::PlayMovie, so this is the single choke point
// that has to know about the replacement; nothing upstream needs touching.
// ---------------------------------------------------------------------------

namespace {

// -1 = nothing has spoken for it yet, so nucleus.ini's gfx.movie_hd decides.
// A loaded profile / the shell checkbox overwrite it via TMovie::SetHDEnabled.
int _hdEnabledState = -1;

// UA_FMV=0 kills the redirect for the whole run, whatever the ini or the
// profile say. Read once — the answer cannot change mid-run.
bool HDKilledByEnv()
{
    static int killed = -1;

    if (killed < 0)
    {
        const char *v = getenv("UA_FMV");
        killed = (v && (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' || *v == 'F')) ? 1 : 0;

        if (killed)
            printf("[FMV] UA_FMV=%s: hd-fmv redirect disabled for this run\n", v);
    }

    return killed != 0;
}

bool DiskFileExists(const std::string &path)
{
    struct stat st;

    if (stat(path.c_str(), &st) != 0)
        return false;

    return (st.st_mode & S_IFMT) == S_IFREG;
}

// "Data\mov\intro.mpg", "mov:intro.mpg" -> "intro.mpg"
std::string MovieBaseName(const std::string &fname)
{
    size_t pos = fname.find_last_of("/\\:");
    return (pos == std::string::npos) ? fname : fname.substr(pos + 1);
}

// -1 = nothing has spoken for it yet, so nucleus.ini's gfx.movie_scale decides.
// A loaded profile / the shell checkbox overwrite it via TMovie::SetFillEnabled.
int _fillEnabledState = -1;

// UA_FMV_SCALE=max|fill overrides gfx.movie_scale for the whole run. Read once
// — the environment cannot change under us mid-run.
const char *ScaleEnv()
{
    static const char *value = NULL;
    static bool read = false;

    if (!read)
    {
        value = getenv("UA_FMV_SCALE");
        read = true;
    }

    return value;
}

// "fill" is the only word that means crop-to-cover; "max" and anything else
// mean the safe, nothing-lost framing.
bool ScaleWordIsFill(const std::string &word)
{
    std::string v = word;

    for (char &c : v)
        c = (char)tolower((unsigned char)c);

    return v == "fill";
}

// Header-only probe of a movie's frame size. Hidden-window runs never open the
// file for playback, so this is what lets the geometry be logged (and checked)
// without decoding anything: container headers only, no codec opened.
bool ProbeVideoSize(const std::string &diskPath, int *w, int *h)
{
    AVFormatContext *fmt = NULL;

    if (avformat_open_input(&fmt, diskPath.c_str(), NULL, NULL) != 0)
        return false;

    bool ok = false;

    if (avformat_find_stream_info(fmt, NULL) >= 0)
    {
        int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

        if (idx >= 0)
        {
#ifndef OLDCTX
            *w = fmt->streams[idx]->codecpar->width;
            *h = fmt->streams[idx]->codecpar->height;
#else
            *w = fmt->streams[idx]->codec->width;
            *h = fmt->streams[idx]->codec->height;
#endif
            ok = (*w > 0 && *h > 0);
        }
    }

    avformat_close_input(&fmt);

    return ok;
}

}

void TMovie::SetFillEnabled(bool on)
{
    _fillEnabledState = on ? 1 : 0;
}

bool TMovie::FillEnabled()
{
    // The environment wins outright, so a smoke run can flip the mode without
    // touching anybody's profile.
    const char *env = ScaleEnv();

    if (env && *env)
        return ScaleWordIsFill(env);

    // Then the checkbox / the profile it was loaded from.
    if (_fillEnabledState >= 0)
        return _fillEnabledState != 0;

    // Otherwise nucleus.ini seeds the default.
    return FillDefaultFromIni();
}

bool TMovie::FillDefaultFromIni()
{
    return ScaleWordIsFill(System::IniConf::GfxMovieScale.Get<std::string>());
}

MovieScaleMode TMovie::ScaleMode()
{
    // Deliberately NOT FillEnabled(): the checkbox stopped selecting a scale
    // mode when it became a file-variant picker. Both of its states now show
    // the whole frame, because "off" answers with a film that is already 4:3
    // rather than by un-cropping a cropped one at playback time.
    //
    // MOVIE_SCALE_FILL is kept alive on purpose — MovieDestRect still
    // implements it and UA_FMV_SCALE=fill still reaches it — so the crop-to-
    // cover maths stays testable without a UI that can strand a player in it.
    const char *env = ScaleEnv();

    if (env && *env)
        return ScaleWordIsFill(env) ? MOVIE_SCALE_FILL : MOVIE_SCALE_MAX;

    return MOVIE_SCALE_MAX;
}

void TMovie::SetHDEnabled(bool on)
{
    _hdEnabledState = on ? 1 : 0;
}

bool TMovie::HDEnabled()
{
    if (HDKilledByEnv())
        return false;

    if (_hdEnabledState >= 0)
        return _hdEnabledState != 0;

    return System::IniConf::GfxMovieHD.Get<bool>();
}

std::string TMovie::ResolveMoviePath(const std::string &fname, bool *isHD, bool *is43)
{
    if (isHD)
        *isHD = false;

    if (is43)
        *is43 = false;

    if (!HDEnabled())
        return fname;

    std::string base = MovieBaseName(fname);
    size_t dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);

    if (stem.empty())
        return fname;

    std::string lower = stem;
    for (char &c : lower)
        c = (char)tolower((unsigned char)c);

    // A plain relative path off the game CWD, handed straight to libavformat:
    // utils.cpp' correctSeparatorAndExt() would flip the separator and clip a
    // longer extension, and the FSMgr node tree only knows what was on disk at
    // startup. stat() answers for the directory as it is right now.
    static const char * const exts[] = { ".mkv", ".mp4" };

    // "Fullscreen Movies" off wants the uncropped 4:3 spare. Only the eight
    // films that actually lost picture to the crop have one, so this is a
    // preference and not a requirement: tut1, tut2, tut3 and ende fall straight
    // through to the plain stem, whose installed file already is full-frame.
    const bool wantWide = !FillEnabled();

    for (int variant = 0; variant < 2; variant++)
    {
        // variant 0 = the "-43" spare, variant 1 = the shipped cropped film.
        if (variant == 0 && !wantWide)
            continue;

        const char *suffix = (variant == 0) ? "-43" : "";

        for (int pass = 0; pass < 2; pass++)
        {
            const std::string &name = (pass == 0) ? stem : lower;

            if (pass == 1 && lower == stem)
                break;

            for (const char *ext : exts)
            {
                std::string cand = "hd-fmv/" + name + suffix + ext;

                if (DiskFileExists(cand))
                {
                    if (isHD)
                        *isHD = true;
                    if (is43)
                        *is43 = (variant == 0);
                    return cand;
                }
            }
        }
    }

    return fname;
}

void TMovie::PlayMovie(const std::string &fname, int volume)
{
    // Resolve and announce before the hidden-window bail-out below: hidden mode
    // never renders a frame, but an unattended smoke run still has to be able to
    // prove which file the redirect picked.
    bool hd = false;
    bool wide = false;
    std::string path = ResolveMoviePath(fname, &hd, &wide);

    if (hd)
        printf("[FMV] %s -> %s (%s)\n", MovieBaseName(fname).c_str(), path.c_str(),
               wide ? "hd 4:3" : "hd");
    else
        printf("[FMV] %s (original)\n", MovieBaseName(fname).c_str());

    // Geometry next, and deliberately still above the hidden-window bail-out.
    // ProcessFrame is the only place the rect is actually used, and it never
    // runs off screen, so computing and announcing it here is what lets an
    // unattended smoke run prove the scaling maths. The frame size comes from a
    // header-only probe rather than from a decoded frame, for the same reason.
    _ctx->scaleMode = ScaleMode();

    Common::Point win = System::GetResolution();
    int vidW = 0;
    int vidH = 0;

    if (ProbeVideoSize(MovieDiskPath(path, hd), &vidW, &vidH))
    {
        MovieRect dst = MovieDestRect(_ctx->scaleMode, win.x, win.y, vidW, vidH);

        printf("[FMV] scale=%s window=%dx%d video=%dx%d dest=%dx%d offset=(%d,%d)\n",
               (_ctx->scaleMode == MOVIE_SCALE_FILL) ? "fill" : "max",
               win.x, win.y, vidW, vidH, dst.w, dst.h, dst.x, dst.y);
    }
    else
    {
        printf("[FMV] scale=%s window=%dx%d video=? (frame size unavailable)\n",
               (_ctx->scaleMode == MOVIE_SCALE_FILL) ? "fill" : "max", win.x, win.y);
    }

    fflush(stdout);

    // Nothing to watch off screen, and playback owns the main loop until the
    // film ends or a key is pressed — which would hang an unattended pass on
    // the intro movie forever.
    if (System::WindowHidden())
        return;

    MovieGLDump("before-movie");
    _ctx->dumpedFrameState = false;

    if (!OpenFile(path, hd))
    {
        // Same outcome the engine has always had for an unopenable movie: skip
        // it. Say why, so a bad drop-in file is not a silent missing cutscene.
        if (hd)
            printf("[FMV] %s could not be opened, movie skipped\n", path.c_str());

        return;
    }

    SFXEngine::SFXe.AudioStream->stop();
    SFXEngine::SFXe.AudioStream->setMasterVolume(volume);
    _ctx->playing = true;
    
    System::EventsAddHandler(TMovie::EventsWatcher);
    
    for(size_t i = 0; i < 10; i++)
        ReadFrames();
    
    int32_t stime = SDL_GetTicks();
    int32_t nextSync = 0;
    
    while( _ctx->playing )
    {
        int32_t curPts = SDL_GetTicks() - stime;
        if (curPts < 0)
            curPts = 0;
        
        if (!_ctx->fileEOF)
            ReadFrames();
        
        if (_ctx->audioStream >= 0)
        {
            //if (!audFrames.empty() && (SFXEngine::SFXe.AudioStream->DataLeft() < minLeft || nextPA <= curPts) )
            if (!_ctx->audFrames.empty())
                ProcessAudio();
            
            if (curPts / 1000 >= nextSync)
            {
                nextSync = (curPts / 1000) + 1;
                
                int32_t ts = (int32_t)SFXEngine::SFXe.AudioStream->GetTS();
                
                if (ts > 0)
                    stime = (((int32_t)SDL_GetTicks() - ts) + stime) / 2;
            }
        }
        
        if (!_ctx->vidFrames.empty())
            ProcessFrame(curPts);
        
        if (_ctx->fileEOF)
        {
            if (_ctx->vidFrames.empty() && _ctx->audFrames.empty())
            {
                _ctx->playing = false;
                if (_ctx->audioStream >= 0)
                    SDL_Delay(SFXEngine::SFXe.AudioStream->BuffersTime()); //Give time for audio to be played
                else
                    SDL_Delay(50);
            }
        }
        
        if ( System::ProcessEvents() )
        {
            _ctx->playing = false;
            PostQuitMessage();
        }
        
        SDL_Delay(1);
    }
    
    System::EventsDeleteHandler(TMovie::EventsWatcher);
    SFXEngine::SFXe.AudioStream->stop();
    
    Close();

    // Playback owns the GL context for the length of the film: it rewrites the
    // viewport, both matrices, the clear colour and most render states, and
    // restores only GFXEngine's GfxStates struct. Re-stamp the whole pipeline
    // from that struct so the renderer resumes on a context that provably
    // matches the engine's cache, whatever the film touched.
    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    GFX::Engine.SetRenderStates(1);

    MovieGLDump("after-movie");
}


}