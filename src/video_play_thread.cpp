// ***********************************************************/
// video_play_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// Video play thread. This section includes key code for
// synchronizing video frames and audio using pts/dts,
//  as well as subtitle processing.
// ***********************************************************/

#include "video_play_thread.h"

extern int framedrop;

const QRegularExpression VideoPlayThread::m_assFilter = QRegularExpression("{\\\\.*?}");
const QRegularExpression VideoPlayThread::m_assNewLineReplacer = QRegularExpression("\\\\n|\\\\N");

VideoPlayThread::VideoPlayThread(QObject* parent, VideoState* pState)
    : QThread(parent), m_pState(pState)
{
}

VideoPlayThread::~VideoPlayThread()
{
    stop_thread();
    final_resample_param();
}

void VideoPlayThread::run()
{
    assert(m_pState);
    VideoState* is = m_pState;
    double remaining_time = 0.0;

    for (;;)
    {
        if (m_bExitThread)
            break;

        if (is->abort_request)
            break;

        if (is->paused)
        {
            msleep(10);
            continue;
        }

        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));

        remaining_time = REFRESH_RATE;
        if ((!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
    }

    qDebug("-------- Video play thread exit.");
}

void VideoPlayThread::video_refresh(VideoState* is, double* remaining_time)
{
    double time;
    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (is->video_st)
    {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
            *remaining_time = REFRESH_RATE;
        }
        else
        {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            //获取上一帧画面
            lastvp = frame_queue_peek_last(&is->pictq);
            //获取当前帧
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial)
            {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            //计算默认delay,即duration=当前帧pts-上一帧pts
            last_duration = vp_duration(is, lastvp, vp);
            //计算默认delay,内部会获取视频时钟+音频时钟然后做比较，之后跟会根据视频帧是否落后/超前计算出最合适的delay
            delay = compute_target_delay(last_duration, is);
            //获取当前系统时间并进行转换
            time = av_gettime_relative() / 1000000.0;
            //frame_timer本身代表上一帧的预计提交显示时间
            //frame_timer+dealy=当前帧预计提交显示的时间
            //每次计算后frame_timer+=dealy进行迭代
            //情况1：当前系统时间没有到当前帧的预计显示时间
            if (time < is->frame_timer + delay)
            {//重建计算需要等待的时间间隔，让外层循环判断
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }
          //情况2：当前帧的显示时间已经到了，接下来要分两种情况判断：一个是当前帧在合适显示范围内（当前帧-下一帧）显示时间之间，另一种是当前帧晚了
            is->frame_timer += delay;
          //首先检查记录的frame_timer是否错误，如果错误就重新从当前开始记录，避免后续误差积累
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;
      //上锁，更新视频时钟
            is->pictq.mutex->lock();
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            is->pictq.mutex->unlock();
      //只有当队列还有可显示帧时才会判断超时丢帧
            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
            //首先取当前帧的下一帧数据
                Frame* nextvp = frame_queue_peek_next(&is->pictq);
            //计算两者的理论差距：pts相减
                duration = vp_duration(is, vp, nextvp);
            //条件1：允许丢帧
            //条件2：允许丢帧且主时钟不是视频
            //条件3：当前时间超过了本帧提交时间+下一帧的间隔
            //判断条件3时frame_timer已经是本帧预计提交时间了，这里判断当前系统时间有没有超出下一帧的预计显示时间
            //如果超过了就直接丢帧数进入下一轮显示帧循环
                if (!is->step &&
                    (framedrop > 0 ||
                     (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) &&
                    time > is->frame_timer + duration)
                {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }
//处理字幕
//如果存在字幕流
            if (is->subtitle_st)
            {
            //只要还有未消费的字幕，就检查队首字幕是否仍然有效。
            //注意这里是循环处理，会处理队列中全部的字幕
                while (frame_queue_nb_remaining(&is->subpq) > 0)
                {
                //获取当前字幕帧
                    sp = frame_queue_peek(&is->subpq);
                //如果存在下一帧也取出来
                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = nullptr;
//判断条件如下：
//条件1：当前字幕代次为旧代次
//条件2：当前字幕的pts已经超过了显示时间
//条件3：下一条字幕应该开始显示了
//因为字幕紧跟画面，只需要和视频时钟的pts做比较就行，无需额外对比
                    if (sp->serial != is->subtitleq.serial ||
                        (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
                        (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                    {
#if 0
						if (sp->uploaded) {
							int i;
							for (i = 0; i < sp->sub.num_rects; i++) {
								AVSubtitleRect* sub_rect = sp->sub.rects[i];
#endif
//当前帧丢弃
                        frame_queue_next(&is->subpq);
                    }
                    else
                    {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            //标记可以显示
            is->force_refresh = 1;

            if (is->step && !is->paused)
                toggle_pause(is, !is->step);
        }
//进行显示
    display:
        /* display picture *///只有开启了force_fresh标记才会显示当前记录的帧，否则就跳过
        if (is->force_refresh && is->pictq.rindex_shown)
            video_display(is);
    }
//否则显示为0，本次帧没显示
    is->force_refresh = 0;
}

void VideoPlayThread::video_display(VideoState* is)
{
    if (is->audio_st && false)
    {
        // video_audio_display(is);
    }
    else if (is->video_st)
    {
        video_image_display(is); //调用显示函数
    }
}

//该函数是sdl版本的，QT版本弃用
#if 0
void VideoPlayThread::video_audio_display(VideoState* s)
{
	int64_t audio_callback_time = 0;
	int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
	int ch, channels, h, h2;
	int64_t time_diff;
	int rdft_bits, nb_freq;

	for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
		;
	nb_freq = 1 << (rdft_bits - 1);

	/* compute display index : center on currently output samples */
	channels = s->audio_tgt.channels;
	nb_display_channels = channels;
	if (!s->paused) {
		int data_used = (2 * nb_freq);
		n = 2 * channels;
		delay = s->audio_write_buf_size;
		delay /= n;

		/* to be more precise, we take into account the time spent since
		   the last buffer computation */
		if (audio_callback_time) {
			time_diff = av_gettime_relative() - audio_callback_time;
			delay -= (time_diff * s->audio_tgt.freq) / 1000000;
		}

		delay += 2 * data_used;
		if (delay < data_used)
			delay = data_used;

		i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
		/*if (s->show_mode == SHOW_MODE_WAVES) {
			h = INT_MIN;
			for (i = 0; i < 1000; i += channels) {
				int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
				int a = s->sample_array[idx];
				int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
				int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
				int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
				int score = a - d;
				if (h < score && (b ^ c) < 0) {
					h = score;
					i_start = idx;
				}
			}
		}*/

		s->last_i_start = i_start;
	}
	else {
		i_start = s->last_i_start;
	}

#if 0
	if (s->show_mode == SHOW_MODE_WAVES) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

		/* total height for one channel */
		h = s->height / nb_display_channels;
		/* graph height / 2 */
		h2 = (h * 9) / 20;
		for (ch = 0; ch < nb_display_channels; ch++) {
			i = i_start + ch;
			y1 = s->ytop + ch * h + (h / 2); /* position of center line */
			for (x = 0; x < s->width; x++) {
				y = (s->sample_array[i] * h2) >> 15;
				if (y < 0) {
					y = -y;
					ys = y1 - y;
				}
				else {
					ys = y1;
				}
				fill_rectangle(s->xleft + x, ys, 1, y);
				i += channels;
				if (i >= SAMPLE_ARRAY_SIZE)
					i -= SAMPLE_ARRAY_SIZE;
			}
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

		for (ch = 1; ch < nb_display_channels; ch++) {
			y = s->ytop + ch * h;
			fill_rectangle(s->xleft, y, s->width, 1);
		}
	}
	else {
		if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
			return;

		if (s->xpos >= s->width)
			s->xpos = 0;
		nb_display_channels = FFMIN(nb_display_channels, 2);
		if (rdft_bits != s->rdft_bits) {
			av_rdft_end(s->rdft);
			av_free(s->rdft_data);
			s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
			s->rdft_bits = rdft_bits;
			s->rdft_data = av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data));
		}
		if (!s->rdft || !s->rdft_data) {
			av_log(nullptr, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
			s->show_mode = SHOW_MODE_WAVES;
		}
		else {
			FFTSample* data[2];
			SDL_Rect rect = { .x = s->xpos, .y = 0, .w = 1, .h = s->height };
			uint32_t* pixels;
			int pitch;
			for (ch = 0; ch < nb_display_channels; ch++) {
				data[ch] = s->rdft_data + 2 * nb_freq * ch;
				i = i_start + ch;
				for (x = 0; x < 2 * nb_freq; x++) {
					double w = (x - nb_freq) * (1.0 / nb_freq);
					data[ch][x] = s->sample_array[i] * (1.0 - w * w);
					i += channels;
					if (i >= SAMPLE_ARRAY_SIZE)
						i -= SAMPLE_ARRAY_SIZE;
				}
				av_rdft_calc(s->rdft, data[ch]);
			}
			/* Least efficient way to do this, we should of course
			 * directly access it but it is more than fast enough. */
			if (!SDL_LockTexture(s->vis_texture, &rect, (void**)&pixels, &pitch)) {
				pitch >>= 2;
				pixels += pitch * s->height;
				for (y = 0; y < s->height; y++) {
					double w = 1 / sqrt(nb_freq);
					int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
					int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
						: a;
					a = FFMIN(a, 255);
					b = FFMIN(b, 255);
					pixels -= pitch;
					*pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
				}
				SDL_UnlockTexture(s->vis_texture);
			}
			SDL_RenderCopy(renderer, s->vis_texture, nullptr, nullptr);
		}
		if (!s->paused)
			s->xpos++;
	}
#endif
}
#endif

//显示画面的函数
void VideoPlayThread::video_image_display(VideoState* is)
{
// 当前准备处理的字幕。
    Frame* sp = nullptr;
// 获取当前应该显示的视频帧。
//前面的 video_refresh() 已经调用 frame_queue_next()，
// 将选中的帧变为 keep_last 保留帧，
// 因此这里使用 peek_last()，而不是 peek()。
    Frame* vp = frame_queue_peek_last(&is->pictq);
  // 获取提前准备好的图像转换资源：
    Video_Resample* pResample = &m_Resample;

//// ────────── 第一部分：处理字幕 ──────────
// // 有未消费的字幕，才需要检查。
    if (frame_queue_nb_remaining(&is->subpq) > 0)
    {
    //查看队首字幕，不移动队列。
        sp = frame_queue_peek(&is->subpq);
    // 判断当前视频帧是否已经到达字幕开始时间。
    // vp->pts、sp->pts：秒
    // start_display_time：相对字幕时间基点的毫秒偏移
        if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000))
        {
        ////当前字幕没有加载过，如果之前加载过就跳过，同一个字幕可能对应多个视频帧
        //所以在视频帧显示前我们才会循环剔除队列中过期的字幕，一般字幕显示后不会消耗队列里的帧
        //通过判断超时才会提出超时的字幕帧，而不是像视频一样，显示一帧就消费一帧
            if (!sp->uploaded)
            {
                // uint8_t* pixels[4];
                // int pitch[4];
                //判断宽高，一般情况下宽高都是合理的
                if (!sp->width || !sp->height)
                {
                    sp->width = vp->width;
                    sp->height = vp->height;
                }
#if 1
//循环检查，因为一个字幕帧可能存在多个字母区域即num_rects的数量代表字幕区域数量
                for (unsigned int i = 0; i < sp->sub.num_rects; i++)
                {
                //获取低i个字幕区域
                    AVSubtitleRect* sub_rect = sp->sub.rects[i];
                //当前仅支持ASS类型的字幕，所以判断类型
                    if (sub_rect->type == SUBTITLE_ASS)
                    {
                        qDebug("subtitle[%d], format:%d, type:%d, text:%s, flags:%d", i,
                               sp->sub.format, sub_rect->type, sub_rect->text, sub_rect->flags);
                        // QString ass = QString::fromUtf8(sub_rect->ass);
                        //将字幕信息转换成字符串类型
                        QString ass = QString::fromLocal8Bit(QString::fromStdString(sub_rect->ass).toUtf8());
                        //按逗号分割成字符数组
                        QStringList assList = ass.split(",");
                        if (assList.size() > 8) //一般字幕内容存储在第9个里面
                        {
                            ass = assList[8];//取第九个字符串
                            // qDebug("ass: %s", qUtf8Printable(ass));
                            parse_subtitle_ass(ass); //进入提交字幕函数，将字幕交给QT，内部会清理一些字符参数和转移字符规范化
                        }
                    }
                    else
                    {//不是ASS类型的字幕，提出警告
                        qWarning("not handled yet, type:%d", sub_rect->type);
                    }
                }
#else
                for (i = 0; i < sp->sub.num_rects; i++)
                {
                    AVSubtitleRect* sub_rect = sp->sub.rects[i];

                    sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                    sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                    sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                    sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                    is->sub_convert_ctx = sws_getCachedContext(
                        is->sub_convert_ctx, sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                        sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA, 0, nullptr, nullptr,
                        nullptr);
                    if (!is->sub_convert_ctx)
                    {
                        av_log(nullptr, AV_LOG_FATAL,
                               "Cannot initialize the conversion context\n");
                        return;
                    }
#if 1
                    sws_scale(is->sub_convert_ctx, (const uint8_t* const*)sub_rect->data,
                              sub_rect->linesize, 0, sub_rect->h, pixels, pitch);
#else
                    if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect,
                                         (void**)pixels, pitch))
                    {
                        sws_scale(is->sub_convert_ctx,
                                  (const uint8_t* const*)sub_rect->data,
                                  sub_rect->linesize, 0, sub_rect->h, pixels, pitch);
                        SDL_UnlockTexture(is->sub_texture);
                    }
#endif
                }
#endif
//标记字幕已处理，下次进来如果字幕没被超时清除就不用再处理了
                sp->uploaded = 1;
            }
        }
        else
        {
            sp = nullptr;
        }
    }
  // ────────── 第二部分：转换视频像素格式 ──────────
  //创建接收帧rgb转换后的
    AVFrame* pFrameRGB = pResample->pFrameRGB; // dst
  //获取视频上下文
    AVCodecContext* pVideoCtx = is->viddec.avctx;
  //获取当前帧的avframe指针
    AVFrame* pFrame = vp->frame;
//进行图像转换，转换成rgb格式
//pResample->sws_ctx转换上下文
//(uint8_t const* const*)pFrame->data 源图像各平面
//pFrame->linesize源图像各平面的行跨度
//0 从第 0 行开始
//pVideoCtx->height 处理整张图像    pFrameRGB->data目标像素平面 （）处理完后存到pfrmaergb对象里  pFrameRGB->linesize 目标行跨度
    sws_scale(pResample->sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0,
              pVideoCtx->height, pFrameRGB->data, pFrameRGB->linesize);
//之后创建QImage对象作为接收容器
    QImage img(pVideoCtx->width, pVideoCtx->height, QImage::Format_RGB888);
//拷贝字节，按行拷贝
//img.scanLine(y) 起始地址第y行的七点
//pFrameRGB->data[0] + y * pFrameRGB->linesize[0] 代表拷贝数据的指针
//拷贝字节就是宽度，注意拷贝的起始地址按linesize[0]计算因为存在内存对齐
//而真实有效的数据是按width的长度算的，治理一定要注意
    for (int y = 0; y < pVideoCtx->height; ++y)
    {
        memcpy(img.scanLine(y), pFrameRGB->data[0] + y * pFrameRGB->linesize[0], pVideoCtx->width * 3);
    }
// 发出图像信号，由界面层接收并更新显示。
    emit frame_ready(img);
}

//初始化图像转换参数和上下文对象
bool VideoPlayThread::init_resample_param(AVCodecContext* pVideo, bool bHardware)
{
    Video_Resample* pResample = &m_Resample;
    if (pVideo)
    {
        enum AVPixelFormat pix_fmt = pVideo->pix_fmt; // frame format after decode
        if (bHardware)
            pix_fmt = AV_PIX_FMT_NV12;
//按照video上下文的参数初始化转化上下文
//原版图像类型  pix_fmt = AV_PIX_FMT_NV12
//转换目标图像类型  AV_PIX_FMT_RGB24
        struct SwsContext* sws_ctx = sws_getContext(pVideo->width, pVideo->height,
                                                    pix_fmt, // AV_PIX_FMT_YUV420P
                                                    pVideo->width, pVideo->height,
                                                    AV_PIX_FMT_RGB24, // sws_scale destination color scheme
                                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
//初始化帧接收对象
        AVFrame* pFrameRGB = av_frame_alloc();
        if (!pFrameRGB)
        {
            printf("Could not allocate rgb frame.\n");
            return false;
        }
        //获取原版图像的字节数
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pVideo->width, pVideo->height, 32);
        //计算缓冲区大小
        uint8_t* const buffer_RGB = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
        if (!buffer_RGB)
        {
            printf("Could not allocate buffer.\n");
            return false;
        }
//这是关键一步，让avframe和buffer_rgb建立内存映射
//因为av_frame不是存储像素数据的对象，他只是记录了存放的地址
//真实存放数据的是缓冲区buffer_rgb，所以这里要建立映射
//传入对应参数即可
        av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer_RGB, AV_PIX_FMT_RGB24, pVideo->width, pVideo->height, 32);
//保存资源，让后续每次显示视频帧时重复使用。
        pResample->sws_ctx = sws_ctx;
        pResample->pFrameRGB = pFrameRGB;
        pResample->buffer_RGB = buffer_RGB;
        return true;
    }
    return false;
}

//释放图像转换上下文函数
void VideoPlayThread::final_resample_param()
{
    Video_Resample* pResample = &m_Resample;
    // Free video resample context
    sws_freeContext(pResample->sws_ctx);

    // Free the RGB image
    av_free(pResample->buffer_RGB);
    av_frame_free(&pResample->pFrameRGB);
    av_free(pResample->pFrameRGB);
}

//线程暂停
void VideoPlayThread::stop_thread()
{
    m_bExitThread = true;
    wait();
}

void VideoPlayThread::parse_subtitle_ass(const QString& text)
{
    QString str = text;
// 删除 ASS 样式控制标记，例如 {\i1}、{\i0}、{\an8}。
    str.remove(m_assFilter);
// 将字幕中的字面字符 \n、\N 转换为真正的换行。
    str.replace(m_assNewLineReplacer, "\n");
// 删除文本首尾的空白字符，保留正文内部的空格与换行。
    str = str.trimmed();
//发出字幕信号，由QT显示字幕
    emit subtitle_ready(str);
}
