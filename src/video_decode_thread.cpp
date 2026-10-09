// ***********************************************************/
// video_decode_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// Video decode thread. This section includes queues
// and dxva2 hardware transmit decoded frame.
// ***********************************************************/

#include "video_decode_thread.h"

VideoDecodeThread::VideoDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

VideoDecodeThread::~VideoDecodeThread()
{
}


//这个函数就是循环取帧然后存到帧队列
//如果不考虑滤镜，那么核心操作很简单最主要就是调用两个封装好的函数：
//get_video_frame内部会循环取帧，直到取到一个合法的帧作为输出。
//queue_picture函数会将帧加入到对应的队列中实现帧存储。
void VideoDecodeThread::run()
{
    assert(m_pState);
    //获取state对象
    VideoState* is = m_pState;
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();
    AVFrame* tmp_frame = nullptr;
    //记录播放时间戳
    double pts;
    //记录该帧播放时长
    double duration;
    //记录函数调用返回值
    int ret;
    //记录时间戳
    AVRational tb = is->video_st->time_base;
    //记录当前帧率，通过函数进行近似估算
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, nullptr);

//如果开启了滤镜，就加入下面代码初始化滤镜相关参数
#if USE_AVFILTER_VIDEO
    // AVFilterGraph* graph = nullptr;
    //创建滤镜的输出端上下文和输入端上下文
    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    //记录上一帧的宽和高
    int last_w = 0;
    int last_h = 0;
    //记录格式
    enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
    //记录代次
    int last_serial = -1;
    //记录滤镜编号
    int last_vfilter_idx = 0;
#endif
    if (!frame)
        return;

//死循环开始读取包-送包给解码器-从解码器拿帧-处理帧-存入到队列
    for (;;)
    {
        /*if (is->abort_request)
            break;*/
//首先调用全局函数，内部进入循环尝试获取一帧合法的帧，满足时间戳输出条件
//循环内部会先尝试从解码器拿一个帧，然后根据返回值判断。如果少包，那么进入送包逻辑，之后再次循环去取帧
//如果是读取完了，那么我们就结束循环
//如果出错了就暂停循环报错
//读取成功我们会判断当前帧的时间戳有没有落后，如果落后直接丢弃进入下一轮循环
//直到找到一个符合标准的帧返回
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;
//下面处理滤镜逻辑：
#if USE_AVFILTER_VIDEO
//首先检查输入滤镜的参数是否合法,即当前参数是否需要重启滤镜
//如果上一次滤镜参数和这次一致，那么就用上一次的滤镜处理，节省时间
//如果不一致那么可能要考虑修正参数
        if (last_w != frame->width || last_h != frame->height ||
            last_format != frame->format || last_serial != is->viddec.pkt_serial ||
            last_vfilter_idx != is->vfilter_idx || is->req_vfilter_reconfigure)
        {//写入日志输出，通知帧数据发生了变化
            av_log(
                nullptr, AV_LOG_DEBUG,
                "Video frame changed from size:%dx%d format:%s serial:%d to "
                "size:%dx%d format:%s serial:%d\n",
                last_w, last_h,
                (const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"),
                last_serial, frame->width, frame->height,
                (const char*)av_x_if_null(
                    av_get_pix_fmt_name((AVPixelFormat)frame->format), "none"),
                is->viddec.pkt_serial);
            //先释放掉之前的graph
            avfilter_graph_free(&is->vgraph);
            //重新分配graph管理滤镜
            is->vgraph = avfilter_graph_alloc();
            if (!is->vgraph)
            {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            //让滤镜框架自动决定线程数量。这里指的是滤镜内部的线程配置，与当前 VideoDecodeThread 不是同一个概念。
            is->vgraph->nb_threads = 0;
            //建立滤镜连接根据当前帧的参数和滤镜描述，建立处理链：
            //这个函数封装了滤镜初始化到建立连接的逻辑
            if ((ret = configure_video_filters(is->vgraph, is, is->vfilters, frame)) <
                0)
            {
                goto the_end;
            }
            //保存滤镜配置结果：当前未启用
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = (AVPixelFormat)frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);

            is->req_vfilter_reconfigure = 0;
        }
//如果没有变化，那么不需要重新修改滤镜相关的参数
//直接向滤镜添加帧
        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;
//如果ret》0说明向滤镜提交帧成功，那么我们需要循环尝试取一个合法的帧出来
        while (ret >= 0)
        {
        //获取一个上次一拿到帧的时间
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;
        //开始取帧
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0)
            {//如果是读取完成，那么就直接返回
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }
          //计算滤镜所消耗的时间，用于后续从解码器取帧时的判断
            is->frame_last_filter_delay =
                av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
                //如果测到的值异常大，则不使用这个估计，防止它导致过度丢帧。
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            //取得滤镜输出端的时间基。后面解释处理后的帧时间戳时，应使用输出端的单位。
            tb = av_buffersink_get_time_base(filt_out);
#endif

#if 0
            duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#else
        //默认准备将 frame 入队
        //我们暂时将tmp—frame=frame
        tmp_frame = frame;
        //如果帧=DXVA2 硬件帧，先传到系统内存
        if (frame->format == AV_PIX_FMT_DXVA2_VLD) // DXVA2 hardware decode frame
        {
        //硬件帧它与普通 YUV 软件帧不同，数据主要通过硬件表面表示，后续普通 CPU 图像处理不能直接把它当作常规像素缓冲使用。
            ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret < 0)
            {
                av_log(nullptr, AV_LOG_WARNING, "Error transferring the data to system memory\n");
                goto the_end;
            }
            // sw_frame->hw_frames_ctx = frame->hw_frames_ctx;
            sw_frame->pts = frame->pts;
            sw_frame->pkt_dts = frame->pkt_dts;
            //处理好后最后再赋值回tmp_frame 保证统一
            tmp_frame = sw_frame;
        }
        //到这里我们手里有一个完整的帧，可能是没经过滤镜处理的，也可能是滤镜处理好的
        //这里转换帧的播放时长为秒，然后获取pts。之后传入queue_picture将帧存放到state内部全局的帧队列中完成解码操作
        duration = (frame_rate.num && frame_rate.den ? av_q2d({frame_rate.den, frame_rate.num}) : 0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(is, tmp_frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
        //释放掉临时资源
        av_frame_unref(tmp_frame);
#endif

#if USE_AVFILTER_VIDEO
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }
#endif

        if (ret < 0)
            goto the_end;
    }

the_end:

#if USE_AVFILTER_VIDEO
    avfilter_graph_free(&is->vgraph);
    if (is->vfilters)
    {
        av_free(is->vfilters);
        is->vfilters = nullptr;
    }
#endif
//释放掉相关资源引用
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
    qDebug("-------- video decode thread exit.");
    return;
}
