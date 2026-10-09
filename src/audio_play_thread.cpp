// ***********************************************************/
// audio_play_thread.cpp
//
//      Copy Right @ Steven Huang. All rights reserved.
//
// audio play thread
// ***********************************************************/

#include "audio_play_thread.h"

#if !NDEBUG
#define DEBUG_PLAYFILTER 0
#define WRITE_AUDIO_FILE 0
#else
#define DEBUG_PLAYFILTER 0
#define WRITE_AUDIO_FILE 0
#endif

#if WRITE_AUDIO_FILE
#include <fstream>
#endif

AudioPlayThread::AudioPlayThread(QObject* parent, VideoState* pState) : QThread(parent), m_pState(pState)
{
    print_device();//输出设备信息，方便检查机器有哪些音频设备。
    qRegisterMetaType<AudioData>("AudioData");
    //注册自定义类型，供跨线程的排队信号传递使用。后面发送音频可视化数据时，会用到 AudioData。
}

AudioPlayThread::~AudioPlayThread()
{
    // stop_thread();
    stop_device();
    final_resample_param();
}

//
void AudioPlayThread::print_device() const
{
//取得默认输出设备。
    auto audioOutput = QMediaDevices::defaultAudioOutput();
    audioOutput.description();

//分别遍历输入设备和输出设备，再调用辅助函数打印详细信息。
    for (const auto& device : QMediaDevices::audioInputs())
        audio_device_detail(device);

    for (const auto& device : QMediaDevices::audioOutputs())
        audio_device_detail(device);
}

//
void AudioPlayThread::audio_device_detail(const QAudioDevice& device) const
{
//首先获取device的类型
    auto mode = device.mode();
    //输入设备，例如麦克风
    if (mode == QAudioDevice::Mode::Input)
    {
        qDebug() << "Input audio device: ";
    }
    else if (mode == QAudioDevice::Mode::Output)//音频输出设备，例如耳机/扬声器
    {
        qDebug() << "Output audio device: ";
    }
    else
    {
        qDebug() << "Warning, NULL audio device! ";
        return;
    }
//打印相关参数
    qDebug() << "desc: " << device.description();
    qDebug() << ", ID: " << device.id();
    qDebug() << ", Mode: " << device.mode();
    qDebug() << ", IsDefault: " << device.isDefault();
//
    QAudioFormat fmt = device.preferredFormat();
}

//初始化设备，根据传入的参数构造QAudioFormat ，告诉QT详细的音频参数
//参数包括，采样率，声道，采样格式
bool AudioPlayThread::init_device(int sample_rate, int channel, AVSampleFormat sample_fmt, float default_vol)
{
//首先获取输出设备的信息
    auto deviceInfo = QMediaDevices::defaultAudioOutput();
//创建格式对象
    QAudioFormat format;
//设置参数，根据传入的实参赋值
    format.setSampleRate(sample_rate);
    format.setChannelCount(channel);
    format.setSampleFormat(QAudioFormat::Int16);
    /*format.setSampleSize(8 * av_get_bytes_per_sample(sample_fmt));
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);*/

    // qDebug("sample size=%d\n", 8 * av_get_bytes_per_sample(sample_fmt));
    //如果音频输出不支持这个参数，就报错返回失败
    if (!deviceInfo.isFormatSupported(format))
    {
        qWarning() << "Raw audio format not supported!";
        return false;
    }
//创建QAudioSink对象，该对象专门用于接收pcm数据帧
//通过独占智能指针管理，传入设备信息和数据格式两个参数实现初始化
    m_pOutput = std::make_unique<QAudioSink>(deviceInfo, format);
//设置音量，为默认音量
    set_device_volume(default_vol);
//启动设备，并把videosink交给m_audioDevice进行管理
//QIODevice* m_audioDevice{nullptr};
//start后可以通过write提交数据进行播放了
    m_audioDevice = m_pOutput->start();
    return true;
}

//返回当前输出的音量大小
float AudioPlayThread::get_device_volume() const
{
    if (m_pOutput)
        return m_pOutput->volume();

    return 0;
}

//设置当前音量大小
void AudioPlayThread::set_device_volume(float volume)
{
    if (m_pOutput)
        m_pOutput->setVolume(volume);
}

//停止设备，直接通过管理对象调用stop即可
void AudioPlayThread::stop_device()
{
    if (m_pOutput)
    {
        m_pOutput->stop();
        m_pOutput->reset();
    }
}

//这是一种兜底方案，正常不会走这个路径直接播放文件的
//如果是纯音频播放可能会考虑走这个路径
void AudioPlayThread::play_file(const QString& file)
{
    /*play pcm file directly*/
    //创建文件对象
    QFile audioFile;
    //将对象绑定对应的文件路径
    audioFile.setFileName(file);
    //只读打开
    audioFile.open(QIODevice::ReadOnly);
    //直接播放
    m_pOutput->start(&audioFile);
}

//线程主循环调用的函数，真正的播放音频帧数据
//传入数据缓冲区指针和大小
void AudioPlayThread::play_buf(const uint8_t* buf, int datasize)
{
    if (!m_audioDevice)
        return;

    uint8_t* data = (uint8_t*)buf;
    //通过死循环写入数据
    while (datasize > 0)
    {
    //调用write进行写入，如果写入数据<0就break
        qint64 len = m_audioDevice->write((const char*)data, datasize);
        if (len < 0)
            break;
        if (len > 0)
        {
            data = data + len; //做指针移动，下次写入会从正确位置开始写入
            datasize -= len; //做减法，记录大小
        }
        // qDebug("play buf:reslen:%d, write len:%d", len, datasize);
    }
}

//线程启动后执行该函数，实现读取播放线程的逻辑
void AudioPlayThread::run()
{
    assert(m_pState);
    //获取播放状态
    VideoState* is = m_pState;
    //记录音频数据的字节数
    int audio_size;

    for (;;)
    {
    //这个播放线程被要求退出。
        if (m_bExitThread)
            break;
    //整个播放过程被要求中止
        if (is->abort_request)
            break;
  //暂停时休眠 10ms，然后重新检查状态，暂时不继续取帧、写数据。
        if (is->paused)
        {
            msleep(10);
            continue;
        }
  //取帧函数负责从帧队列消费一帧数据写入给qt层，并播放该音频数据
  //audio记录了本次帧的字节数
  //内部将帧队列里的数据提交给qt，通过play_buf函数内部调用m_audioDevice->write
        audio_size = audio_decode_frame(is);
        if (audio_size < 0)
            break;
//更新时钟操作
        if (!isnan(is->audio_clock))
        {
        //拿到音频解码器上下文
            AVCodecContext* pAudioCodex = is->auddec.avctx;
            if (pAudioCodex)
            {
            //计算一秒音频数据的大小，会根据声道数，采样率，采样格式来判断
            //bytes_per_sec = 采样率 × 声道数 × 每个采样值的字节数
            //所以该数据的播放时长=audio_size/每秒字节数据
                int bytes_per_sec = av_samples_get_buffer_size(nullptr, pAudioCodex->ch_layout.nb_channels,
                                                               pAudioCodex->sample_rate, AV_SAMPLE_FMT_S16, 1);
          // 记录当前系统相对时间，单位为微秒。
                int64_t audio_callback_time = av_gettime_relative();
          //绑定时钟，
                set_clock_at(&is->audclk, is->audio_clock - (double)(audio_size) / bytes_per_sec,
                             is->audio_clock_serial, audio_callback_time / 1000000.0);
           //exclk校准，这个时钟用于恢复audio时钟
                sync_clock_to_slave(&is->extclk, &is->audclk);
            }
        }
    }

    qDebug("-------- Audio play thread exit.");
}

//解码函数
int AudioPlayThread::audio_decode_frame(VideoState* is)
{
//记录字节数
    int data_size;
//帧对象，接收队列中的帧
    Frame* af;
// 持续取帧，跳过旧代次，直到拿到当前代次的音频帧。
    do//该循环唯一左右就是
    {
    //首先循环等待如果当前队列帧为0，如果文件到末尾了则直接返回
        while (frame_queue_nb_remaining(&is->sampq) == 0)
        {
            if (is->eof)
            {
                // break;
                return -1;
            }
            else
            {
                av_usleep(1000);
                // return -1;
            }

            if (is->abort_request)
                break;
        }
      //调用frame_queue_peek_readable获取一帧
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
      //帧队列指针变化
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);//判断代次

    /*data_size = av_samples_get_buffer_size(nullptr, af->frame->channels,
          af->frame->nb_samples,
          AVSampleFormat(af->frame->format), 1);*/

#if USE_AVFILTER_AUDIO
//开启滤镜时直接拷贝数据之所以能直接复制，是因为上游滤镜已经准备好了目标格式。
//计算这帧音频按 S16 格式存储，需要多少字节
    data_size = av_samples_get_buffer_size(nullptr, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
//开辟字存储字节的缓冲区，通过av_malloc分配指定大小
    uint8_t* const buffer_audio = (uint8_t*)av_malloc(data_size * sizeof(uint8_t));
//数据拷贝，将数据拷贝到临时缓冲区
    memcpy(buffer_audio, af->frame->data[0], data_size);
#else
//如果没有开启滤镜那么只能通过转换上下文对数据进行格式转换：
//创建转换上下文
    struct SwrContext* swrCtx = m_audioResample.swrCtx;
//计算字节答案小
    data_size = av_samples_get_buffer_size(
        nullptr, af->frame->channels, af->frame->nb_samples, AV_SAMPLE_FMT_S16,
        0); // AVSampleFormat(af->frame->format)
//创建缓冲器
    uint8_t* buffer_audio = (uint8_t*)av_malloc(data_size * sizeof(uint8_t));
//对数据进行重采样
    int ret =
        swr_convert(swrCtx, &buffer_audio, af->frame->nb_samples,
                    (const uint8_t**)(af->frame->data), af->frame->nb_samples);
    if (ret < 0)
    {
        return 0;
    }
#endif
//如果静音那么数据就归零
    if (is->muted && data_size > 0)
        memset(buffer_audio, 0, data_size); // mute
//可选，如果选择保存 PCM 文件，打开文件将字节写入到文件中
#if WRITE_AUDIO_FILE
    std::ofstream myfile;
    myfile.open("audio.pcm", std::ios::out | std::ios::app | std::ios::binary);
    if (myfile.is_open())
    {
        myfile.write((char*)buffer_audio, data_size);
    }
#endif
//可选：发送数据用于可视化
    if (m_bSendToVisual)
    {
        AudioData data;
        if (data_size > BUFFER_LEN)
        {
            qDebug() << "audio frame is too long,data_size:" << data_size
                     << ", buffer_len:" << BUFFER_LEN << "\n";
        }

        int len = std::min(data_size, BUFFER_LEN);
        memcpy(data.buffer, buffer_audio, len);
        data.len = len;
        emit data_visual_ready(data);
    }
//调用play——buf将数据写入到qt的media设备
    play_buf(buffer_audio, data_size);
//释放缓冲区大小
    av_free((void*)buffer_audio);

    /* update the audio clock with the pts */
    //更新全局记录的时钟
    if (!isnan(af->pts))
    {
        // is->audio_clock = af->pts + (double)af->frame->nb_samples /
        // af->frame->sample_rate;
        //计算当前音频的时间
        double frame = (double)af->frame->nb_samples / af->frame->sample_rate;
        // frame = frame * is->audio_speed;
        //将时间记录到时钟里
        is->audio_clock = af->pts + frame;
#if USE_AVFILTER_AUDIO
        is->audio_clock = is->audio_clock_old + (is->audio_clock - is->audio_clock_old) * is->audio_speed;
        // is->audio_clock = is->audio_clock * is->audio_speed;
#endif

#if DEBUG_PLAYFILTER
        static int pks_num = 0;
        pks_num++;

        qDebug("[%d]audio: clock=%0.3f pts=%0.3f, (nb:%d, sr:%d)frame:%0.3f\n",
               pks_num, is->audio_clock, af->pts, af->frame->nb_samples,
               af->frame->sample_rate, frame);

        // qDebug("audio: clock=%0.3f pts=%0.3f, (nb:%d, sr:%d)frame:%0.3f\n",
        // is->audio_clock, af->pts, af->frame->nb_samples, af->frame->sample_rate,
        // frame);
#endif
    }
    else
    {
        is->audio_clock = NAN;
    }
    is->audio_clock_serial = af->serial;

    emit update_play_time();

#if (!NDEBUG && PRINT_PACKETQUEUE_AUDIO_INFO)
    {
        static double last_clock;
        qDebug("audio: delay=%0.3f clock=%0.3f\n", is->audio_clock - last_clock, is->audio_clock);
        last_clock = is->audio_clock;
    }
#endif

    return data_size;
}


//初始化参数：创建并初始化音视频转换上下文，用于写入数据时的转换
bool AudioPlayThread::init_resample_param(AVCodecContext* pAudio, AVSampleFormat sample_fmt, VideoState* is)
{
    if (pAudio)
    {
        int ret = -1;
        struct SwrContext* swrCtx = nullptr;
#if USE_AVFILTER_AUDIO
        if (is)
        {
            AVFilterContext* sink = is->out_audio_filter;
            // int sample_rate = av_buffersink_get_sample_rate(sink);
            // int nb_channels = av_buffersink_get_channels(sink);

            AVChannelLayout channel_layout;
            av_buffersink_get_ch_layout(sink, &channel_layout);
            // int64_t channel_layout = av_buffersink_get_channel_layout(sink);
            int format = av_buffersink_get_format(sink);

            ret = swr_alloc_set_opts2(&swrCtx, &pAudio->ch_layout, sample_fmt,
                                      pAudio->sample_rate, &channel_layout,
                                      (AVSampleFormat)format, pAudio->sample_rate, 0,
                                      nullptr);

            /*m_audioResample.channel_layout = channel_layout;
      m_audioResample.sample_fmt = sample_fmt;
      m_audioResample.sample_rate = pAudio->sample_rate;*/
        }
#else
        ret = swr_alloc_set_opts2(&swrCtx, &pAudio->ch_layout, sample_fmt,
                                  pAudio->sample_rate, &pAudio->ch_layout,
                                  pAudio->sample_fmt, pAudio->sample_rate, 0,
                                  nullptr);
#endif

        if (!(ret < 0))
        {
            swr_init(swrCtx);
            m_audioResample.swrCtx = swrCtx;
            return true;
        }
    }
    return false;
}

void AudioPlayThread::final_resample_param()
{
    swr_free(&m_audioResample.swrCtx);
}

void AudioPlayThread::stop_thread()
{
    m_bExitThread = true;
    wait();
}
