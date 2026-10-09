// ***********************************************************/
// packets_sync.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// packets A/V synchronization struct and operations definition.
// This code is referenced from ffplay.c in Ffmpeg library.
// ***********************************************************/

#include "packets_sync.h"

int framedrop = -1;
// static int decoder_reorder_pts = -1;
// static int display_disable = 1;
// static int64_t audio_callback_time;

int packet_queue_init(PacketQueue* q)
{
//首先清空数据
    memset(q, 0, sizeof(PacketQueue));
    //开辟可动态变化大小的存储MYAVpktlist对象的queue容器，avfioi类型，类似于queue<myavpacketlist>
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = new QMutex(); //初始化锁
    if (!q->mutex)
    {
        av_log(nullptr, AV_LOG_FATAL, "new QMutex() error.\n");
        return AVERROR(ENOMEM);
    }
    q->cond = new QWaitCondition(); //初始化条件变量
    if (!q->cond)
    {
        av_log(nullptr, AV_LOG_FATAL, "new QWaitCondition() error.\n");
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1; //初始化状态值
    return 0;
}
//队列销毁函数
void packet_queue_destroy(PacketQueue* q)
{
    packet_queue_flush(q); //首先清空队列中的pkt
    av_fifo_freep2(&q->pkt_list);//释放内存
    delete q->mutex;//删除锁
    delete q->cond;//删除条件变量
}

void packet_queue_flush(PacketQueue* q)
{
    MyAVPacketList pkt1; 

    q->mutex->lock();
    //循环遍历整个队列，然后调用av_pkt——free释放资源
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0; //下列是相关数据的归零
    q->size = 0;
    q->duration = 0;
    q->serial++; //只要刷新，代次++防止读取/处理旧包
    q->mutex->unlock(); //解锁
}

//初始化队列代次参数
void packet_queue_start(PacketQueue* q)
{
    q->mutex->lock(); //启动队列，加锁
    q->abort_request = 0; //修改暂停标志为0
    q->serial++; //代次首次启动是1
    q->mutex->unlock();
}

void packet_queue_abort(PacketQueue* q)
{
    q->mutex->lock();
    q->abort_request = 1;
    q->cond->wakeAll();
    q->mutex->unlock();
}

//从队列中取包
int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial)
{
#if PRINT_PACKETQUEUE_INFO
    // packet_queue_print(q, pkt, "packet_queue_get");
#endif

    MyAVPacketList pkt1;
    int ret = 0;
//加锁
    q->mutex->lock();

    for (;;)
    {
    //如果暂停则返回
        if (q->abort_request)
        {
            ret = -1;
            break;
        }
      //我们读取第一个数据成功
        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        {
        //读取成功，计数--
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            //总时长--
            q->duration -= pkt1.pkt->duration;
            //转移资源
            av_packet_move_ref(pkt, pkt1.pkt);
            //获取代次
            if (serial)
                *serial = pkt1.serial;
                //释放临时变量
            av_packet_free(&pkt1.pkt);
            ret = 1;
            //终止循环
            break;
        }
        else if (!block)
        {
            ret = 0;
            break;
        }
        else
        {
            q->cond->wait(q->mutex);
        }
    }
    q->mutex->unlock();
    //返回结果
    return ret;
}

void packet_queue_print(const PacketQueue* q, const AVPacket* pkt, const QString& prefix)
{
    qDebug("[%s]Queue:[%p](nb_packets:%d, size:%d, dur:%d, serial:%d), "
           "pkt(pts:%lld,dts:%lld,size:%d,s_index:%d,dur:%lld,pos:%lld).",
           qUtf8Printable(prefix), q, q->nb_packets, q->size, q->duration,
           q->serial, pkt->pts, pkt->dts, pkt->size, pkt->stream_index,
           pkt->duration, pkt->pos);
}

//将AVPacket递交给某一个队列PacketQueue
//整体思路很简单：先创建pkt1接通过move转移资源-加锁-调用packet_queue_put_print入队-解锁-释放pkt1
//packet_queue_put_private内部就是封装成MYPacketlist对象（）*pkt+serial,z再入队，然后增加计数，时长字节等相关数据
//最后返回入队结果，此时资源的指针被队列维护
int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
    AVPacket* pkt1;
    int ret = -1;
  //分配一个新的avpacket对象
    pkt1 = av_packet_alloc();
    if (!pkt1)
    {
        av_packet_unref(pkt);
        return ret;
    }
    //通过move转移资源，这里pkt资源被转移，同时pkt被置空
    av_packet_move_ref(pkt1, pkt);
    //加锁，调用队列内部的入队函数
    q->mutex->lock();
    //入队，并接收是否入队成功
    ret = packet_queue_put_private(q, pkt1);
    //解锁
    q->mutex->unlock();
    //判断是否入队成功
    if (ret < 0)
        av_packet_free(&pkt1);

#if PRINT_PACKETQUEUE_INFO
        // packet_queue_print(q, pkt, "packet_queue_put");
#endif
    //返回结果 1表示入队成功，-1表示入队失败
    return ret;
}

int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

//通用函数，将数据入队
//考虑到生产者只有一个线程，不需要加锁
int packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
{
//创建自定义的队列对象，内部维护了以一个引用计数和avpacket对象
    MyAVPacketList pkt1;
    int ret;
  
    if (q->abort_request)
        return -1;
  //存储当前包数据
    pkt1.pkt = pkt;
    //存当前队列的代次
    pkt1.serial = q->serial;
  //真正将包写入到队列中
    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    //队列计数++
    q->nb_packets++;
    //计算字节大小，累加
    q->size += pkt1.pkt->size + sizeof(pkt1);
    //计算时长
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    //通知等待线程存在一个数据包
    q->cond->wakeAll();
    return 0;
}

//初始化帧队列
//帧队列内部维护了一系列的下标+avframe指针
int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
{
    int i;
    //首先将数据清空
    memset(f, 0, sizeof(FrameQueue));
    //初始化互斥锁
    if (!(f->mutex = new QMutex()))
    {
        av_log(nullptr, AV_LOG_FATAL, "new QMutex() error!\n");
        return AVERROR(ENOMEM);
    }
    //初始化条件变量
    if (!(f->cond = new QWaitCondition()))
    {
        av_log(nullptr, AV_LOG_FATAL, "new QWaitCondition() error\n");
        return AVERROR(ENOMEM);
    }
    //给packet队列赋值
    f->pktq = pktq;
    //计算一下最大大小，取较小的
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    //循环给每个frmaequeued队列的对象初始化
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

//销毁帧队列
void frame_queue_destory(FrameQueue* f)
{
    int i;
    //循环释放每个frame
    for (i = 0; i < f->max_size; i++)
    {
    //拿到第i项，类似于int  *p=  &a[1]
        Frame* vp = &f->queue[i];
        //释放内部的avframe和sub，因为只有这两个是堆区内存，需要手动管理释放，否则内存泄露。
        frame_queue_unref_item(vp);
        //然后释放vp这个对象，这个对象除了前面两个其他直接删除就行。
        av_frame_free(&vp->frame);
    }
//删除互斥锁和条件变量
    delete f->mutex;
    delete f->cond;
}

//处理拿到frame对象后，释放其内部的avframe和sub
//专门设计释放frmae内存的函数
void frame_queue_unref_item(Frame* vp)
{
    av_frame_unref(vp->frame); // frame reference number reduce 1
    avsubtitle_free(&vp->sub); // sub
}

//条件变量通知全部一次：唤醒等待队列变化的全部线程
void frame_queue_signal(FrameQueue* f)
{
    f->mutex->lock();
    f->cond->wakeAll();
    f->mutex->unlock();
}

//返回当前待消费帧的指针，拿到的永远是待消费的帧
//该函数返回的位置永远是：f->rindex + f->rindex_shown
//rindex指向的永远是队列的头部，只是这一帧可能是上次取走但保留的，也可能是未取走预计取走的帧。
//所以通过rindex——show来实现到底取哪个
//show==1说明保留帧了，show==0说明没保存
//保存了rindex+1就是待消费帧，没保存rindex就是待消费帧
Frame* frame_queue_peek(FrameQueue* f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

//返回待消费的下一帧
Frame* frame_queue_peek_next(FrameQueue* f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

//如果上一帧保留了，z这个函数就可以获取上次保留的帧
Frame* frame_queue_peek_last(FrameQueue* f) { return &f->queue[f->rindex]; }

//获取可写位置的地址
Frame* frame_queue_peek_writable(FrameQueue* f)
{
    /* wait until we have space to put a new frame */
    f->mutex->lock();
    //条件判断+wait,如果队列满了才阻塞
    while (f->size >= f->max_size && !f->pktq->abort_request)
    {
        f->cond->wait(f->mutex);
    }
    f->mutex->unlock();

    if (f->pktq->abort_request)
        return nullptr;
//返回当前写指针
    return &f->queue[f->windex];
}

//获取可读位置地址，返回一帧可消费的帧
Frame* frame_queue_peek_readable(FrameQueue* f)
{
    /* wait until we have a readable a new frame */
    f->mutex->lock();
    //如果当前大小-rindex_shown =目前队列中没读的数量，不包括度过但保存的帧
    //小于=0说明没数据读，直接wait
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request)
    {
        f->cond->wait(f->mutex);
    }
    f->mutex->unlock();

    if (f->pktq->abort_request)
        return nullptr;
  //返回对应位置的值
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

//写入完后会调用这个函数
//将写好的帧加入队列，说白了就是增加计数，更新下标，唤醒等待的消费者线程
void frame_queue_push(FrameQueue* f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    f->mutex->lock();
    f->size++;
    f->cond->wakeAll();
    f->mutex->unlock();
}

//读取完后会调用这个函数
//分两种情况：保留/不保留
//保留则更新show的值直接返回
//不保留，则释放当前位置的对象，更新rindex的值，减少size计数，通知等待的写入线程
void frame_queue_next(FrameQueue* f)
{
    if (f->keep_last && !f->rindex_shown)
    {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    f->mutex->lock();
    f->size--;
    f->cond->wakeAll();
    f->mutex->unlock();
}

/* return the number of undisplayed frames in the queue */
//返回未读的帧，不包含消费单没释放的
int frame_queue_nb_remaining(FrameQueue* f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
//返回保留帧的上一个位置，前提是代次和当前队列代次一致
//这个返回保留帧只有在特定条件下才会保留，例如show==1的情况
//注意返回的是帧在文件的位置不是在队列的位置
int64_t frame_queue_last_pos(FrameQueue* f)
{
    Frame* fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

//封装整体最终函数，将帧加入到帧队列里，上面的内容都是服务这个函数的
//对外暴露的就是这个接口
//is保存了本次播放对话 的状态，用于管理本次播放的各个对象，例如AVFormatcontext和decoder 和stream等
//is内部还包括帧队列，包队列等缓冲区容器
int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
#if PRINT_PACKETQUEUE_INFO
    // int64 lld, double lf
    qDebug("queue picture, w:%d, h:%d, nb:%d, ft:%d(%s), kf:%d, pic_t:%d(%c), "
           "pts:%lf, duration:%lf, pos:%lld, serial:%d",
           src_frame->width, src_frame->height, src_frame->nb_samples,
           src_frame->format,
           av_get_sample_fmt_name(AVSampleFormat(src_frame->format)),
           src_frame->key_frame, src_frame->pict_type,
           av_get_picture_type_char(src_frame->pict_type), pts, duration, pos,
           serial);
#endif
//将传入的信息封装成我们自定义的feame类型：
    Frame* vp;
  //先入队，拿到当前可写地址，指向一个容器中空的地址
    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;
  //像素宽高比
    vp->sar = src_frame->sample_aspect_ratio;
  //将相关显示状态恢复为尚未上传。
    vp->uploaded = 0;
  //图像尺寸
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    //图像像素格式
    vp->format = src_frame->format;
    //记录该帧的时间戳
    vp->pts = pts;
    //记录该帧的持续时间
    vp->duration = duration;
    //记录该帧的文件位置
    vp->pos = pos;
    //记录代次
    vp->serial = serial;
    //转移所有权!!这里完成了帧的存储
    av_frame_move_ref(vp->frame, src_frame);
    //调用push函数成功写入，更新计数+通知线程
    frame_queue_push(&is->pictq);
    return 0;
}

//解码一个视频帧，并决定是否因落后而丢弃,关键是判断是否丢帧
//is内部包含解码器，时钟等用于判断是否丢帧
int get_video_frame(VideoState* is, AVFrame* frame)
{
    int got_picture = -1;
//首先调用通用解码函数拿到一个输出的视频帧
//这个函数拿到了真正的帧，frame指向了avframe的地址
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, nullptr)) < 0)
        return -1;

    if (got_picture)
    {
  //首先设置显示时间戳为一个无效值，这样可以避免没有时间戳的帧被误认为是第0秒
        double dpts = NAN;
  //做时间戳转换 真实播放时间=时间基*时间戳
  //av_q2d就是把分数转换成double的函数
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
  //拿到帧的一个像素宽高比，注意是每个像素的，有时像素不一定是正方形
        frame->sample_aspect_ratio =
            av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);
  //判断是否允许提前丢弃帧,if条件是判断是否开启了提前丢帧的模式，值得注意的是
  //如果同步策略是视频为基准，那么就不允许丢帧，必须保证视频的完整
        if (framedrop > 0 ||
            (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
        {
        //判断时间戳是否合法
            if (frame->pts != AV_NOPTS_VALUE)
            {
            //将当前显示时间-主时钟时间得到diff差值，然后根据diff的正负进行判断
            //diff<0说明当前帧落后于主时钟
            //diff=0说明正好
            //diff>0说明超前于主时钟
            //之后根据超前/落后的大小进行判断，看其是否超出了阈值
            //isnan(diff) 判断该插值是否是个有效值
            //fabs(diff) < AV_NOSYNC_THRESHOLD这个判断插值是否异常大，如果很大则判断为异常情况不应该当作丢帧情况去处理
            //diff - is->frame_last_filter_delay < 0  
            //1，在没有滤镜处理延时的条件下，只要这个帧落后于时钟了，就丢弃。
            //2，在开启滤镜处理延时的条件下，落后仍然丢弃，但是对于超前帧可能也会以丢弃，当diff-delay<0说明延迟高于超前的时间，这个帧预计后续会落后，也是直接丢弃。
            //可以说只要落后且满足条件，就会丢帧，。
            //is->viddec.pkt_serial == is->vidclk.serial代次必须一致，防止输出旧帧
            //is->videoq.nb_packets后面还有pkt输入数据，也就是说不允许丢帧后出现没数据的情况
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets)
                {
                //丢帧计数++
                    is->frame_drops_early++;
                //释放frame引用，返回空
                    av_frame_unref(frame);
                    //返回值为0
                    got_picture = 0;
                }
            }
        }
    }
  //返回最终结果，此时形参frame真实指向了帧的内存
    return got_picture;
}

//初始化自己封装的解码器对象
int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, QWaitCondition* empty_queue_cond)
{
    //清空编码器内存
    memset(d, 0, sizeof(Decoder));
    //临时变量，用于不断取出队列里的包
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
        //获取解码器上下文
    d->avctx = avctx;
        //获取包队列
    d->queue = queue;
        //获取条件变量
    d->empty_queue_cond = empty_queue_cond;
    //起始时间戳赋值未知
    d->start_pts = AV_NOPTS_VALUE;
    //代次默认-1
    d->pkt_serial = -1;
    return 0;
}
//启动解码器：
int decoder_start(Decoder* d, void* thread, const char* thread_name)
{
//开启队列，初始化代次参数，并没有启动的含义就是更新代次和其他参数
    packet_queue_start(d->queue);
    //保存线程指针
    d->decoder_tid = thread;
    //拿到解码器名词字符串
    d->decoder_name = av_strdup(thread_name);
    return 0;
}

void decoder_destroy(Decoder* d)
{
    //销毁内部临时变量
    av_packet_free(&d->pkt);
    //解引用解码器上下文
    avcodec_free_context(&d->avctx);
    //释放存储的字符串
    av_free(d->decoder_name);
}

//终止函数
void decoder_abort(Decoder* d, FrameQueue* fq)
{
//更新队列状态为abort
    packet_queue_abort(d->queue);
    //向消费者线程发送信号，通知暂停
    frame_queue_signal(fq);
    // SDL_WaitThread(d->decoder_tid, nullptr);
    //解码线程阻塞，等待退出，因为重新启动会传入新的thread对象，重新绑定
    ((QThread*)(d->decoder_tid))->wait();
    //id置空，线程退出后置空id
    d->decoder_tid = nullptr;
    //清除包内容+代次++,暂停一次后之前的数据都属于旧数据了
    packet_queue_flush(d->queue);
}

//解码器作用，反复协调取包，送包和取帧，直到有结果/需要退出
int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub)
{
    int ret = AVERROR(EAGAIN);
    int decoder_reorder_pts = -1;
    for (;;)
    {
    //第一层判断，代次一定要相同
        if (d->queue->serial == d->pkt_serial)
        {
        //阶段一：尝试获取一个已有的帧
        //这个循环负责从 FFmpeg 解码器中接收结果。
            do
            {
                if (d->queue->abort_request)
                    return -1;
                //判断解码器类型
                switch (d->avctx->codec_type)
                {
                //视频类型
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);//调用recv尝试从解码器获取一个视频帧
                        if (ret >= 0)//获取成功
                        {//接下来修正pts
                            if (decoder_reorder_pts == -1)
                            {
                                frame->pts = frame->best_effort_timestamp; //安排一个合适时间戳
                            }
                            else if (!decoder_reorder_pts)
                            {
                                frame->pts = frame->pkt_dts; //使用包时间戳
                            }
                        }
                        break;
                        //音频类型
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame); //解码拿到音频帧
                        if (ret >= 0)
                        {
                            AVRational tb = AVRational{1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE) //新 PTS × 新时间基 = 原 PTS × 原时间基
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE) //如果当前帧没有有效 PTS，但之前已经推算过下一帧时间，则使用：
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) //预测下一音频帧的 PTS
                            {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                if (ret == AVERROR_EOF)//avcodec_receive_frame() 返回 AVERROR_EOF 时，表示：解码器已经进入排空状态，并且内部不再有剩余帧。
                {
                    d->finished = d->pkt_serial;//记录完成的是哪个播放代次。
                    avcodec_flush_buffers(d->avctx);//重置解码器内部缓存状态，使上下文后续可以重新开始处理输入。
                    return 0;
                }
                if (ret >= 0) //读出一帧直接返回
                    return 1;
            } while (ret != AVERROR(EAGAIN)); //只要 ret 不是 EAGAIN，循环就继续。   
        }
      //如果走到这里说明上面读取没有成功且因为ret=EAGAIN导致循环暂停，或者说代次发生了变化
      //下面我需要进行给解码器塞pkt让其解码出新的帧
        do
        {
        //如果队列为空，那么通过条件变量通知所有等待为空的线程唤醒
            if (d->queue->nb_packets == 0)
                d->empty_queue_cond->wakeAll();
      
            if (d->packet_pending)
            {
                d->packet_pending = 0;//如果存在上一个没有用到的包就先设置成0
                //也就是说d内部的pkt是没提交的所以这里只变packet-pend即可
            }
            else
            {//这里需要取新帧，但是首先要记录当前代次，以免后面代次发生变化
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial)
                {
                //如果不等那么调用清除，清除自身缓存
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;//此时拿到新包直接退出循环
            av_packet_unref(d->pkt);
        } while (1);
      //下面进行sendpkt,如果是字幕，那么字幕和音视频使用不同的解码路线
        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
        //字幕采用下列函数执行send
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0)
            {
                ret = AVERROR(EAGAIN);
            }
            else
            {
                if (got_frame && !d->pkt->data)
                {
                    d->packet_pending = 1;//记录当前
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        }
        else
        {
        //音频/视频采用下列函数执行send
            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN))
            {
                av_log(d->avctx, AV_LOG_ERROR,
                       "Receive_frame and send_packet both returned EAGAIN, which is "
                       "an API violation.\n");
                d->packet_pending = 1;
            }
            else
            {
                av_packet_unref(d->pkt);
            }
        }
    }
}

//解封装第一步，
void get_file_info(const char* filename, int64_t& duration)
{
    AVFormatContext* pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) == 0)
    {
        if (pFormatCtx->duration < 0)
            avformat_find_stream_info(pFormatCtx, NULL);
        duration = pFormatCtx->duration;
    }
    // etc
    avformat_close_input(&pFormatCtx);
    avformat_free_context(pFormatCtx);
}

void get_duration_time(const int64_t duration_us, int64_t& hours, int64_t& mins, int64_t& secs, int64_t& us)
{
    int64_t duration = duration_us + (duration_us <= INT64_MAX - 5000 ? 5000 : 0);
    duration = duration < 0 ? 0 : duration;
    secs = duration / AV_TIME_BASE;
    us = duration % AV_TIME_BASE;
    us = (100 * us) / AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
}

//获取时钟时间
double get_clock(Clock* c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused)
    {
        return c->pts;
    }
    else
    {//当前视频时间=pts+(当前系统时间-记录的系统时间)*speed
    //下面公式实际上带入pts——drift的展开就行 pts_drift=pts-last_update
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}
//设置时钟，讲系统时间和播放时间进行关联，用于后续推理其他帧播放时间
void set_clock_at(Clock* c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}
//作用和上面一样，只是这里自动取当前系统时间
void set_clock(Clock* c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}
//修改速度重新绑定两个时间，然后单独修改倍速属性即可
void set_clock_speed(Clock* c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}
//初始化时钟，注意这里代次和默认的播放时钟一个是-1一个是NAN都是无效值
//默认播放时钟不能是0，因为有的时候播放开始的时间戳并不是0s开始
void init_clock(Clock* c, int* queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

//将第一个时钟的时间按照第二个时钟时间进行校准
void sync_clock_to_slave(Clock* c, Clock* slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) &&
        (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

//判对应类型队列是否存在重组的pkt包
int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue)
{
    return stream_id < 0 || queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES &&
               (!queue->duration ||
                av_q2d(st->time_base) * queue->duration > 1.0);
}

int is_realtime(AVFormatContext* s)
{
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp"))
        return 1;

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

//获取当前同步策略，作为主时钟。当前默认音频主时钟
int get_master_sync_type(VideoState* is)
{
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    }
    else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
    {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    }
    else
    {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
//获取主时钟的时间
double get_master_clock(VideoState* is)
{
    double val;

    switch (get_master_sync_type(is))
    {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

void check_external_clock_speed(VideoState* is)
{
    if ((is->video_stream >= 0 &&
         is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (is->audio_stream >= 0 &&
         is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES))
    {
        set_clock_speed(&is->extclk,
                        FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                              is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 ||
              is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (is->audio_stream < 0 ||
              is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES))
    {
        set_clock_speed(&is->extclk,
                        FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                              is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else
    {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP *
                                                     (1.0 - speed) /
                                                     fabs(1.0 - speed));
    }
}

/* seek in the stream */
void stream_seek(VideoState* is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req)
    {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        // SDL_CondSignal(is->continue_read_thread);
        is->continue_read_thread->wakeAll();
    }
}

/* pause or resume the video */
// void stream_toggle_pause(VideoState* is, bool pause)
//{
//	if (is->paused) {
//		is->frame_timer += av_gettime_relative() / 1000000.0 -
//is->vidclk.last_updated; 		if (is->read_pause_return != AVERROR(ENOSYS)) {
//			is->vidclk.paused = 0;
//		}
//		set_clock(&is->vidclk, get_clock(&is->vidclk),
//is->vidclk.serial);
//	}
//	set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
//	is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused =
//pause; // !is->paused; 	is->step = 0;
//}

void toggle_pause(VideoState* is, bool pause)
{
    if (is->paused)
    {
        is->frame_timer +=
            av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS))
        {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused =
        pause; // !is->paused;
    is->step = 0;
}

void toggle_mute(VideoState* is, bool mute)
{
    bool muted = !!is->muted;
    if (muted != mute)
    {
        is->muted = mute;
    }
}

void update_volume(VideoState* is, int sign, double step)
{
    double volume_level =
        is->audio_volume
            ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10))
            : -1000.0;
    int new_volume =
        lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(
        is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume,
        0, SDL_MIX_MAXVOLUME);
}

void step_to_next_frame(VideoState* is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        toggle_pause(is, !is->paused);
    is->step = 1;
}

double compute_target_delay(double delay, VideoState* is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)
    {
        /* if video is slave, we try to correct big delays by
       duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know
       if it is the best guess */
        sync_threshold =
            FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
    return delay;
}

double vp_duration(VideoState* is, Frame* vp, Frame* nextvp)
{
    if (vp->serial == nextvp->serial)
    {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    }

    return 0.0;
}

void update_video_pts(VideoState* is, double pts, int64_t pos, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

#if PRINT_PACKETQUEUE_INFO
void print_state_info(VideoState* is)
{
    if (is)
    {
        PacketQueue* pPacket = &is->videoq;
        qDebug("[VideoState] V PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        pPacket = &is->audioq;
        qDebug("[VideoState] A PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        pPacket = &is->subtitleq;
        qDebug("[VideoState] S PacketQueue[%p](nb_packets:%d,size:%d,dur:%lld, "
               "abort:%d, serial:%d)",
               pPacket, pPacket->nb_packets, pPacket->size, pPacket->duration,
               pPacket->abort_request, pPacket->serial);

        /*qDebug("[VideoState]FrameQueue(v:%p,a:%p,s:%p)",
            &is->pictq, &is->sampq, &is->subpq);
    qDebug("[VideoState]Decoder(v:%p,a:%p,s:%p)",
            &is->viddec, &is->auddec, &is->subdec);
    qDebug("[VideoState]Clock(v:%p,a:%p,s:%p)",
            &is->vidclk, &is->audclk, &is->extclk);*/
    }
}
#endif

#if USE_AVFILTER_AUDIO
void set_audio_playspeed(VideoState* is, double value)
{
    if (value < 0 || value > 4)
        return;

    if (is->audio_speed == value)
        return;

    is->audio_speed = value;

    const size_t len = 32;
    if (!is->afilters)
        is->afilters = (char*)av_malloc(len);

    if (value <= 0.5)
    {
        snprintf(is->afilters, len, "atempo=0.5,");
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "atempo=%lf", value / 0.5);

        // strncat(is->afilters, tmp, len - strlen(is->afilters) - 1);
        strncat_s(is->afilters, len, tmp, len - strlen(is->afilters) - 1);
    }
    else if (value <= 2.0)
    {
        snprintf(is->afilters, len, "atempo=%lf", value);
    }
    else
    {
        snprintf(is->afilters, len, "atempo=2.0,");
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "atempo=%lf", value / 2.0);
        // strncat(is->afilters, tmp, len - strlen(is->afilters) - 1);
        strncat_s(is->afilters, len, tmp, len - strlen(is->afilters) - 1);
    }

    qDebug("changing audio filters to :%s", is->afilters);

#if USE_AVFILTER_VIDEO
    set_video_playspeed(is);
#endif

    is->audio_clock_old = is->audio_clock;
    is->req_afilter_reconfigure = 1;
}

void set_video_playspeed(VideoState* is)
{
    double speed = is->audio_speed;

    size_t len = 32;
    if (!is->vfilters)
        is->vfilters = (char*)av_malloc(len);

    snprintf(is->vfilters, len, "setpts=%.4lf*PTS", 1.0 / speed);

    is->req_vfilter_reconfigure = 1;

    qDebug("changing video filters to :%s", is->vfilters);
}

int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

// int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
//{
//	if (channel_layout && av_get_channel_layout_nb_channels(channel_layout)
//== channels) 		return channel_layout; 	else 		return 0;
//}


//根据图和参数配置滤镜
//首先这个函数传入了四个参数
//参数一是管理滤镜的图对象，用于管理接收滤镜上下文，滤镜上下文，输出滤镜上下文
//这个函数的作用是将三个上下文建立滤镜流并交给滤镜图上下文管理
//char* filtergraph描述中间处理流程的字符串，如 "atempo=2.0"，本质就是一个滤镜字符串参数
int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph, AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
{
    int ret;
    int nb_filters = graph->nb_filters;
    //创建inpu output对象，这是专属的输入输出滤镜上下文对象，专门管理用于输入输出的AVFilterContext，可以通过outputs->filter_ctx 进行赋值
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph)
    {
        //初始化上下文
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs)
        {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        //初始化当前输入参数：将参数source_ctx,赋值给in管理
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        //初始化输出参数：将参数sink_ctx,赋值给in管理
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        //解析字符串，创建并连接中间滤镜
        //这个函数会解析字符串并创建中间滤镜，同时完成连接
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs,
                                            nullptr)) < 0)
            goto fail;
    }
    else
    {//如果没有自定义滤镜，那么就连接score和sink即可，不需要中间滤镜，即不做任何处理
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged
   * first */
    for (unsigned int i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i],
               graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, nullptr);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

int configure_audio_filters(VideoState* is, const char* afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16,
                                                      AV_SAMPLE_FMT_NONE};
    int sample_rates[2] = {0, -1};
    // int64_t channel_layouts[2] = { 0, -1 };
    // AVChannelLayout channel_layouts[2] = {};
    // int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    // char aresample_swr_opts[512] = "";
    // const AVDictionaryEntry* e = nullptr;
    char asrc_args[256];
    int ret;
    AVBPrint bp;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = 0;

    /*while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
          av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:",
  e->key, e->value); if (strlen(aresample_swr_opts))
          aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
  av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);*/

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

//构造滤镜参数字符串
    snprintf(asrc_args, sizeof(asrc_args),
             "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
             is->audio_filter_src.freq,
             av_get_sample_fmt_name(is->audio_filter_src.fmt), 1,
             is->audio_filter_src.freq, bp.str);
//创建滤镜上下文
//buffer表述该滤镜作为输入滤镜
    ret = avfilter_graph_create_filter(
        &filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer", asrc_args,
        nullptr, is->agraph);
    if (ret < 0)
        goto end;
//创建输出滤镜上下文，作为接收滤镜输出帧的对象
    ret = avfilter_graph_create_filter(
        &filt_asink, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
        nullptr, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,
                                   AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) <0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1,
                              AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format)
    {
        sample_rates[0] = is->audio_filter_src.freq;
        // channel_layouts[0] = is->audio_filter_src.channel_layout;
        // channels[0] = is->audio_filter_src.channels;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0,
                                  AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;

        if ((ret = av_opt_set(filt_asink, "ch_layouts", bp.str,
                              AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        /*if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts",
       channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;*/
        /*if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1,
       AV_OPT_SEARCH_CHILDREN)) < 0) goto end;*/
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1,
                                       AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc,
                                     filt_asink)) < 0)
        goto end;

    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);
    return ret;
}

int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
{
    enum AVPixelFormat pix_fmts[1]; // FF_ARRAY_ELEMS(sdl_texture_format_map)
    // char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = nullptr, *filt_out = nullptr,
                    *last_filter = nullptr;
    AVCodecParameters* codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, nullptr);
    // const AVDictionaryEntry* e = nullptr;
    int nb_pix_fmts = 0;

    /*
  int i, j;
  for (i = 0; i < renderer_info.num_texture_formats; i++) {
          for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
                  if (renderer_info.texture_formats[i] ==
  sdl_texture_format_map[j].texture_fmt) { pix_fmts[nb_pix_fmts++] =
  sdl_texture_format_map[j].format; break;
                  }
          }
  }*/
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    /*while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
          if (!strcmp(e->key, "sws_flags")) {
                  av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:",
  "flags", e->value);
          }
          else
                  av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:",
  e->key, e->value);
  }
  if (strlen(sws_flags_str))
          sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

  graph->scale_sws_opts = av_strdup(sws_flags_str);*/

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num,
             FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d",
                    fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(
             &filt_src, avfilter_get_by_name("buffer"), "ffplay_buffer",
             buffersrc_args, nullptr, graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(
        &filt_out, avfilter_get_by_name("buffersink"), "ffplay_buffersink",
        nullptr, nullptr, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,
                                   AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

#if 0
	/* Note: this macro adds a filter before the lastly added filter, so the
	 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                                    \
    do                                                                            \
    {                                                                             \
        AVFilterContext* filt_ctx;                                                \
                                                                                  \
        ret = avfilter_graph_create_filter(&filt_ctx, avfilter_get_by_name(name), \
                                           "ffplay_" name, arg, nullptr, graph);  \
        if (ret < 0)                                                              \
            goto fail;                                                            \
                                                                                  \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                         \
        if (ret < 0)                                                              \
            goto fail;                                                            \
                                                                                  \
        last_filter = filt_ctx;                                                   \
    } while (0)

	if (autorotate) {
		int32_t* displaymatrix = (int32_t*)av_stream_get_side_data(is->video_st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
		double theta = get_rotation(displaymatrix);

		if (fabs(theta - 90) < 1.0) {
			INSERT_FILT("transpose", "clock");
		}
		else if (fabs(theta - 180) < 1.0) {
			INSERT_FILT("hflip", nullptr);
			INSERT_FILT("vflip", nullptr);
		}
		else if (fabs(theta - 270) < 1.0) {
			INSERT_FILT("transpose", "cclock");
		}
		else if (fabs(theta) > 1.0) {
			char rotate_buf[64];
			snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
			INSERT_FILT("rotate", rotate_buf);
		}
	}
#endif

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

#if 0
int audio_open(void* opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams* audio_hw_params)
{
	//SDL_AudioSpec wanted_spec, spec;
	//const char* env;
	static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
	static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
	//int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

	/*env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env) {
		wanted_nb_channels = atoi(env);
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
	}*/

	if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}

	wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	/*
	wanted_spec.channels = wanted_nb_channels;
	wanted_spec.freq = wanted_sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
		return -1;
	}
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
		next_sample_rate_idx--;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = opaque;
	while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
		av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
			wanted_spec.channels, wanted_spec.freq, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
		if (!wanted_spec.channels) {
			wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
			wanted_spec.channels = wanted_nb_channels;
			if (!wanted_spec.freq) {
				av_log(nullptr, AV_LOG_ERROR,
					"No more combinations to try, audio open failed\n");
				return -1;
			}
		}
		wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(nullptr, AV_LOG_ERROR,
			"SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) {
		wanted_channel_layout = av_get_default_channel_layout(spec.channels);
		if (!wanted_channel_layout) {
			av_log(nullptr, AV_LOG_ERROR,
				"SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}*/

	audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
	audio_hw_params->freq = wanted_sample_rate; // spec.freq;
	audio_hw_params->channel_layout.nb_channels = wanted_channel_layout;
	audio_hw_params->channels = wanted_nb_channels; // spec.channels;

	audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
	audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
	if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		return -1;
	}
	return 0;// spec.size;
}
#endif

#endif
