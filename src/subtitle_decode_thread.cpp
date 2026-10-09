// ***********************************************************/
// subtitle_decode_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// Subtitles decode thread.
// ***********************************************************/

#include "subtitle_decode_thread.h"

SubtitleDecodeThread::SubtitleDecodeThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

SubtitleDecodeThread::~SubtitleDecodeThread()
{
}
//字幕线程不需要处理滤镜逻辑，所以较为简单
void SubtitleDecodeThread::run()
{
    assert(m_pState);
    VideoState* is = m_pState;
    Frame* sp;
    int got_subtitle;
    double pts = 0;

    for (;;)
    {//先找到队列的可写入位置，拿到指针sp
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return;
      //通过decoder_decode_frame进行取帧逻辑，内部循环取帧拿到一个可用帧返回
        if ((got_subtitle = decoder_decode_frame(&is->subdec, nullptr, &sp->sub)) < 0)
            break;
        
        pts = 0;
      //如果读取成功
        if (got_subtitle && sp->sub.format == 1)
        {
        //检查格式，填写时间戳和代次
            if (sp->sub.pts != AV_NOPTS_VALUE)//计算时间戳
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts; //赋值播放时间戳
            sp->serial = is->subdec.pkt_serial; //记录代次
            sp->width = is->subdec.avctx->width; //记录宽
            sp->height = is->subdec.avctx->height; //记录高
            sp->uploaded = 0; 

            /* now we can update the picture count */
            frame_queue_push(&is->subpq); //逻辑入队，增加计数
        }
        else if (got_subtitle)
        {
            qWarning("Not handled subtitle type:%d", sp->sub.format);
            avsubtitle_free(&sp->sub);
        }
    }

    qDebug("-------- subtitle decode thread exit.");
    return;
}
