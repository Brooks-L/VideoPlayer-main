// ***********************************************************/
// read_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// read packets thread.
// ***********************************************************/

#include "read_thread.h"

extern int infinite_buffer;
extern int64_t start_time;
static int64_t duration = AV_NOPTS_VALUE;

ReadThread::ReadThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pPlayData(pState)
{
}

ReadThread::~ReadThread()
{
}
//存储全局的state
void ReadThread::set_video_state(VideoState* pState)
{
    assert(pState);
    m_pPlayData = pState;
}

//核心代码，该线程会执行这个函数，内部循环进行读包
//
int ReadThread::loop_read()
{
    int ret = -1;
    //首先拿到videostate对象
    VideoState* is = m_pPlayData;
    //创建空包用于接收读取的包
    AVPacket* pkt = nullptr;
    //记录这个包是否满足指定播放范围
    int pkt_in_play_range = 0;
    //当前包所属流的起始时间
    int64_t stream_start_time = 0;
    //用于检查播放范围的包时间戳
    int64_t pkt_ts = 0;
    // AVFormatContext* pFormatCtx = is->ic;
    assert(is);
    // assert(pFormatCtx);
    if (!is)
        return ret;
  //给临时包分配内存
    pkt = av_packet_alloc();
    if (!pkt)
    {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        return ret;
    }
    //记录当前读线程的状态为启动
    is->read_thread_exit = 0;
    //死循环
    for (;;)
    {
    //abort_request 是播放层的退出请求，例如用户停止播放时，其他代码会设置这个标记。这里只是在每轮开始时检查，不代表设置标记就能立即打断正在执行的阻塞读取。
        if (is->abort_request)
            break;
//从播放变为暂停：调用 av_read_pause()。
//从暂停变为播放：调用 av_read_play()。
        if (is->paused != is->last_paused)
        {
        //这里记录前后两个状态，如果前一个状态不是停止，现在这个状态是停止。那么就可以进行停止逻辑
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(is->ic);//调用读线程暂停
            else
                av_read_play(is->ic);//否则就播放
        }
        //主要用于支持这类控制的网络流，例如部分 RTSP 输入；普通本地文件不一定支持。
        //av_read_pause() 和 av_read_play()，它们是 FFmpeg 的输入流暂停、恢复接口：
        //当用户拖动进度条或请求快进、快退时，其他代码会记录跳转参数并设置 seek_req。
        if (is->seek_req)
        {
        //首先记录想调转的位置
            int64_t seek_target = is->seek_pos;
        //这里我们记录一个跳转的范围
            int64_t seek_min =
                is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max =
                is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct
            // direction in generation
            //      of the seek_pos/seek_rel variables
            //调用ffmpegseek进行跳转
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max,
                                     is->seek_flags);
            if (ret < 0)
            {
                av_log(nullptr, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
            }
            else//成功后说明完成跳转，当前队列中的包全部失效。认定为旧数据
            {
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audioq); //清除音频队列数据，更新代次
                if (is->subtitle_stream >= 0)
                    packet_queue_flush(&is->subtitleq);//清除字幕队列数据，更新代次
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);//清除视频队列数据。更新代次
                if (is->seek_flags & AVSEEK_FLAG_BYTE)
                {//如果按照字节位置跳转，则不能直接等同于播放时间，知道目标时间，把外部时钟设置到该时间。
                    set_clock(&is->extclk, NAN, 0);
                }
                else
                {//如果是按照时间跳转的，直接进行转换即可
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;//跳转成功后将跳转标记置为0
            is->queue_attachments_req = 1;//要求重新处理封面图等附带图片
            is->eof = 0; //清除之前的输入结束状态。如果输入结束，发生跳转就需要清空这个标记
            if (is->paused)
                step_to_next_frame(is); //如果暂停中跳转，安排单步播放，让画面有机会更新到跳转后的内容。
        }
      //处理媒体自带的封面图标，有些媒体的“视频流”其实是封面图，例如音乐文件中的专辑封面。
      //AV_DISPOSITION_ATTACHED_PIC 就用于标记这种附带图片。
        if (is->queue_attachments_req)
        {
            if (is->video_st &&
                is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            {
            //这里让 pkt 引用封面图的压缩数据。它与 av_packet_move_ref() 不同：这里是建立引用，原来的 attached_pic 仍然保留。
                ret = av_packet_ref(pkt, &is->video_st->attached_pic);
                if (ret < 0)
                    break;
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }
        //在读包之前需要进行判断，
        //1,当前包队列缓存满了，audioq.size + videoq.size + subtitleq.size > MAX_QUEUE_SIZE
        //2，各路流都缓存得足够多
        /* if the queue are full, no need to read more */
        if (infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size >
                 MAX_QUEUE_SIZE ||
             (stream_has_enough_packets(is->audio_st, is->audio_stream,
                                        &is->audioq) &&
              stream_has_enough_packets(is->video_st, is->video_stream,
                                        &is->videoq) &&
              stream_has_enough_packets(is->subtitle_st, is->subtitle_stream,
                                        &is->subtitleq))))
        {
            /* wait 10 ms */
            //如果真的满足等待条件，
            //加锁
            m_waitMutex.lock();
            // SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            //被唤醒或约 10 毫秒超时后，重新获得锁。
            is->continue_read_thread->wait(&m_waitMutex, 10);
            m_waitMutex.unlock();//回到循环开头，重新检查退出、跳转和缓存条件。//所有条件都满足，开始进行读包
            continue;
        }
//所有条件都满足，开始进行读包
        ret = av_read_frame(is->ic, pkt);
        if (ret < 0)
        {
        //读取失败分多种情况：
        //第一种读取已经结束，向相关队列发送一个空包，用于后续取出来向解码器输入一个空包，拿到最后的结果
            if ((ret == AVERROR_EOF || avio_feof(is->ic->pb)) && !is->eof)
            {
            //判断是哪个流，是那个流就向其输入一个空包
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
              //判断是否循环播放，如果是就跳转到开头
                if (is->loop)
                {
                    stream_seek(is, 0, 0, 0);//这个函数只是修改跳转请求，后续会在下一轮循环进行跳转
                }
                else
                {//否则我们记录播放结束，终端读包循环
                    is->eof = 1;
                    break; // added for auto exit read thread
                }
            }
            if (is->ic->pb && is->ic->pb->error)//如果存在底层 I/O 错误
            {
                break;//直接中断
            }
            //其他情况，等待10ms继续尝试下一轮
            m_waitMutex.lock();
            is->continue_read_thread->wait(&m_waitMutex, 10);
            m_waitMutex.unlock();
            continue;
        }
        else
        {
            is->eof = 0; //否则读取包成功，并且也没有结束，输入结束标记清除
        }

        /* check if packet is in play range specified by user, then queue, otherwise
     * discard */
     //这里成功拿到了包，但是需要判断是否在指定的播放范围内：
     //记录当前流的起始时间：
        stream_start_time = is->ic->streams[pkt->stream_index]->start_time;
     //用于比较的包时间戳：优先使用 PTS，没有 PTS 时使用 DTS。
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range =
            duration == AV_NOPTS_VALUE ||
            (pkt_ts -
             (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                        av_q2d(is->ic->streams[pkt->stream_index]->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) /
                        1000000 <=
                ((double)duration / 1000000);
                //时间检测通过，我们分析流类型然后入队即可
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->audioq, pkt);
        }
        else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
                 !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
        {//选中的普通视频流进入视频队列。这里排除封面图，因为封面已经在前面的专门分支处理了。
            packet_queue_put(&is->videoq, pkt);
        }
        else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range)
        {
            packet_queue_put(&is->subtitleq, pkt);
        }
        else
        {
            av_packet_unref(pkt);
        }

        // print_state_info(is);
    }

    is->read_thread_exit = -1;
    av_packet_free(&pkt);//释放包
    return 0;
}

void ReadThread::run()
{
    int ret = loop_read();
    if (ret < 0)
    {
        qDebug("-------- Read packets thread exit, with error=%d\n", ret);
    }
    else
    {
        qDebug("-------- Read packets thread exit.");
    }
}
