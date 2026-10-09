// ***********************************************************/
// audio_decode_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// audio decode thread
// ***********************************************************/

#include "audio_decode_thread.h"

AudioDecodeThread::AudioDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

AudioDecodeThread::~AudioDecodeThread()
{
}

void AudioDecodeThread::run()
{
//首先获取状态+临时接收帧的创建
    assert(m_pState);
    VideoState* is = m_pState;
    AVFrame* frame = av_frame_alloc();
    Frame* af;
#if USE_AVFILTER_AUDIO
//如果开启音频滤镜，那么就初始化音频滤镜相关参数
    int last_serial = -1;
    AVChannelLayout dec_channel_layout; // int64_t
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return;
//dowhile死循环取帧
    do
    {
        /*if (is->abort_request)
            break;*/
        //首先尝试获取一帧，只有返回值<0时才会退出循环，说明取帧出错了
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, nullptr)) < 0)
            goto the_end;
        //如果拿到帧
        if (got_frame)
        {
        //确定时间戳
            tb = AVRational{1, frame->sample_rate};

#if USE_AVFILTER_AUDIO
            // dec_channel_layout = get_valid_channel_layout(frame->channel_layout,
            // frame->ch_layout.nb_channels);
            //确定声道布局
            dec_channel_layout = frame->ch_layout; // frame->channel_layout; //
        //比较音频参数的函数，比较当前参数和上次使用的滤镜是否相同
        //主要比较：采样格式，声道数量，采样率，播放代次
            reconfigure = cmp_audio_fmts(is->audio_filter_src.fmt,
                                         is->audio_filter_src.ch_layout.nb_channels,
                                         AVSampleFormat(frame->format),
                                         frame->ch_layout.nb_channels) ||
                          is->audio_filter_src.ch_layout.nb_channels !=
                              dec_channel_layout.nb_channels ||
                          is->audio_filter_src.freq != frame->sample_rate ||
                          is->auddec.pkt_serial != last_serial;
//除了参数变化，其他模块也可以通过 req_afilter_reconfigure 主动请求重建，例如调整倍速时。
            if (reconfigure || is->req_afilter_reconfigure)
            {
                char buf1[1024], buf2[1024];
                // av_get_channel_layout_string(buf1, sizeof(buf1), -1,
                // is->audio_filter_src.channel_layout);
                // av_get_channel_layout_string(buf2, sizeof(buf2), -1,
                // dec_channel_layout);
                //将声道信息转化成可读字符串
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1,
                                           sizeof(buf1));
                av_channel_layout_describe(&dec_channel_layout, buf2, sizeof(buf2));
                //日志写入信息变化的内容
                av_log(nullptr, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s "
                       "serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels,
                       av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1,
                       last_serial, frame->sample_rate, frame->ch_layout.nb_channels,
                       av_get_sample_fmt_name(AVSampleFormat(frame->format)), buf2,
                       is->auddec.pkt_serial);
              //真正更新配置的是下面逻辑：
              //获取新的音频格式
                is->audio_filter_src.fmt = (AVSampleFormat)frame->format;
              //拷贝新的音声道布局参数
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
              //获取新的采样率
                is->audio_filter_src.freq = frame->sample_rate;
              //获取新的代次
                last_serial = is->auddec.pkt_serial;
              //调用该函数重新配置滤镜
                ret = configure_audio_filters(is, is->afilters, 1);
                if (ret < 0)
                    goto the_end;
                is->req_afilter_reconfigure = 0;
            }
          //向滤镜添加一帧
            ret = av_buffersrc_add_frame(is->in_audio_filter, frame);
            if (ret < 0)
                goto the_end;
      //不断循环读取滤镜中的帧
            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0)
            {
            //记录时间戳
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                //拿到可写位置指针
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;
            //记录相关信息
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos; //AV_CODEC_FLAG_COPY_OPAQUE; //
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d(AVRational{frame->nb_samples, frame->sample_rate});
                //通过move转移引用
                av_frame_move_ref(af->frame, frame);
                //完成写入增加计数
                frame_queue_push(&is->sampq);

#if USE_AVFILTER_AUDIO
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif

#if PRINT_PACKETQUEUE_AUDIO_INFO
            // int64 lld, double lf
            qDebug("queue audio sample, pts:%lf, duration:%lf, pos:%lld, serial:%d",
                   af->pts, af->duration, af->pos, af->serial);
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:

#if USE_AVFILTER_AUDIO
    avfilter_graph_free(&is->agraph);
    if (is->afilters)
    {
        av_free(is->afilters);
        is->afilters = nullptr;
    }
#endif

    av_frame_free(&frame);
    qDebug("-------- audio decode thread exit.");
    return;
}
