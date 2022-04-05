#include "rtsp.client.hpp"
#include "rtsp.error.hpp"

#include <string>
#include <sstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <cstdint>

////////////////////////////////////////////////////////////////////////////////
// ffmpeg header files
////////////////////////////////////////////////////////////////////////////////
#pragma warning(push)
#pragma warning(disable:4819) // file encoding warning
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
}
#pragma warning(pop)

namespace rtsp
{

struct client::implement
{
  client* m_parent;

  AVFormatContext* m_fctx;
  int m_stream_index[AVMEDIA_TYPE_NB];

  AVPacket* m_packet;
  std::int64_t m_duration;
  bool m_eof;

  AVCodecContext* m_cctx;
  AVFrame* m_frame;
  AVStream* m_video_stream;
  AVRational m_time_base;
  AVRational m_frame_rate;

  std::thread m_packet_worker;
  std::thread m_frame_worker;

  std::mutex m_packet_mu;
  std::condition_variable m_packet_cv;
  std::deque<AVPacket*> m_packet_depot;

  std::mutex m_frame_mu;
  std::condition_variable m_frame_cv;
  std::deque<AVFrame*> m_frame_depot;

  std::mutex m_mu;
  std::condition_variable m_cv;
  bool m_need_to_stop;

public:
  implement(client* _parent)
    : m_parent(_parent)
    , m_fctx(nullptr)
    , m_packet(nullptr)
    , m_eof(false)
    , m_duration(AV_NOPTS_VALUE)
    , m_cctx(nullptr)
    , m_frame(nullptr)
    , m_video_stream(nullptr)
    , m_need_to_stop(false)
  {
    _initialize();
  }
  ~implement()
  {
    _release();
  }
  int open(const char* _url)
  {
    int ret = 0;

    AVDictionary* options = nullptr;

    ret = ::av_dict_set(&options, "scan_all_pmts", "1", 0);
    if (ret < 0)
    {
      on_error(ret, "av_dict_set");
      return ec::rtsp_setting_option_is_failed;
    }

    // open input stream, read the header.
    ret = ::avformat_open_input(&m_fctx, _url, nullptr, &options);
    if (ret < 0)
    {
      on_error(ret, "avformat_open_input");
      return ec::rtsp_opening_stream_is_failed;
    }

    // fill codecpar of AVFormatContext.
    ret = ::avformat_find_stream_info(m_fctx, nullptr);
    if (ret < 0)
    {
      on_error(ret, "avformat_find_stream_info");
      return ec::rtsp_finding_stream_info_is_failed;
    }

    for (unsigned i = 0; i < m_fctx->nb_streams; ++i)
    {
      AVStream* st = m_fctx->streams[i];
      AVMediaType type = st->codecpar->codec_type;
      st->discard = AVDISCARD_ALL;
      if (type >= 0 && m_stream_index[type] == AVMEDIA_TYPE_UNKNOWN)
      {
        m_stream_index[type] = i;
      }
    }

    // find best stream.
    if (m_stream_index[AVMEDIA_TYPE_VIDEO] >= 0)
    {
      ret = _open_video_codec();
      if (ret != ec::none)
      {
        return ret;
      }
    }

    ret = ::av_read_play(m_fctx);
    if (ret < 0)
    {
      on_error(ret, "av_read_play");
      return ec::rtsp_starting_read_stream_is_failed;
    }

    m_packet = ::av_packet_alloc();
    if (m_packet == nullptr)
    {
      on_error(AVERROR(ENOMEM), "av_packet_alloc");
      return ec::rtsp_memory_alloc_is_failed;
    }

    m_packet_worker = std::thread(&implement::_packet_loop, this);

    return ec::not_implemented_yet;
  }
  int close()
  {
    {
      std::lock_guard<std::mutex> locker(m_mu);
      m_need_to_stop = true;
    }
    m_cv.notify_all();

    if (m_packet_worker.joinable())
      m_packet_worker.join();


    return ec::not_implemented_yet;
  }

private:
  void _initialize()
  {
    ::avdevice_register_all();

    ::avformat_network_init();

    for (auto& e : m_stream_index)
    {
      e = AVMEDIA_TYPE_UNKNOWN;
    }
  }
  void _release()
  {
    if (m_fctx)
    {
      ::avformat_close_input(&m_fctx);
    }

    if (m_cctx)
    {
      ::avcodec_free_context(&m_cctx);
    }

    if (m_packet)
    {
      ::av_packet_free(&m_packet);
    }

    if (m_frame)
    {
      ::av_frame_free(&m_frame);
    }

    ::avformat_network_deinit();
  }
  int _open_video_codec()
  {
    int ret = 0;

    m_cctx = ::avcodec_alloc_context3(nullptr);
    if (m_cctx == nullptr)
    {
      on_error(AVERROR(ENOMEM), "avcodec_alloc_context3");
      return ec::rtsp_memory_alloc_is_failed;
    }

    ret = ::avcodec_parameters_to_context(m_cctx, m_fctx->streams[m_stream_index[AVMEDIA_TYPE_VIDEO]]->codecpar);
    if (ret < 0)
    {
      on_error(ret, "avcodec_parameters_to_context");
      return ec::rtsp_codec_context_is_failed;
    }
    m_cctx->pkt_timebase = m_fctx->streams[m_stream_index[AVMEDIA_TYPE_VIDEO]]->time_base;

    AVCodec* codec = ::avcodec_find_decoder(m_cctx->codec_id);

    m_cctx->codec_id = codec->id;

    AVDictionary* options = nullptr;
    ::av_dict_set(&options, "threads", "auto", 0);
    ::av_dict_set(&options, "refcounted_frames", "1", 0);

    ret = ::avcodec_open2(m_cctx, codec, &options);
    if (ret < 0)
    {
      on_error(ret, "avcodec_open2");
      return ec::rtsp_codec_open_is_failed;
    }

    m_fctx->streams[m_stream_index[AVMEDIA_TYPE_VIDEO]]->discard = AVDISCARD_DEFAULT;

    std::int64_t start_pts = AV_NOPTS_VALUE;
    int serial = -1;

    int video_index = m_stream_index[AVMEDIA_TYPE_VIDEO];
    m_video_stream = m_fctx->streams[m_stream_index[AVMEDIA_TYPE_VIDEO]];

    m_time_base = m_video_stream->time_base;
    m_frame_rate = ::av_guess_frame_rate(m_fctx, m_video_stream, nullptr);

    m_frame = ::av_frame_alloc();
    if (m_frame == nullptr)
    {
      on_error(AVERROR(ENOMEM), "av_frame_alloc");
      return ec::rtsp_memory_alloc_is_failed;
    }

    m_frame_worker = std::thread(&implement::_frame_loop, this);

    return ec::none;
  }
  void _packet_loop()
  {
    using namespace std::literals::chrono_literals;
    while (true)
    {
      std::unique_lock<std::mutex> locker(m_mu);
      if (m_cv.wait_for(locker, 1ms, [&] { return m_need_to_stop; }))
      {
        break;
      }
      locker.unlock();

      _read_packet();
    }
  }
  int _read_packet()
  {
    int ret = 0;

    ret = ::av_read_frame(m_fctx, m_packet);
    if (ret < 0)
    {
      if ((ret == AVERROR_EOF || ::avio_feof(m_fctx->pb)) && !m_eof)
      {
        m_eof = true;
      }
      if (m_fctx->pb && m_fctx->pb->error)
      {
        on_error(ret, "av_read_frame");
        return ec::rtsp_reading_packet_is_failed;
      }
      return ec::rtsp_packet_seg;
    }
    m_eof = false;

    if (m_packet->stream_index != m_stream_index[AVMEDIA_TYPE_VIDEO])
    {
      ::av_packet_unref(m_packet);
      return ec::rtsp_video_stream_is_only;
    }

    AVStream* video_stream = m_fctx->streams[m_stream_index[AVMEDIA_TYPE_VIDEO]];

    auto start_time = video_stream->start_time;
    auto ts = (m_packet->pts == AV_NOPTS_VALUE ? m_packet->dts : m_packet->pts);
    auto in_play = (m_duration == AV_NOPTS_VALUE ||
                    (ts - (start_time != AV_NOPTS_VALUE ? start_time : 0)) *
                    ::av_q2d(video_stream->time_base) -
                    (double)(start_time != AV_NOPTS_VALUE? start_time : 0) / 1000000
                    <= ((double)m_duration / 1000000));
    if (in_play &&
        !(video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
    {
      AVPacket* packet = ::av_packet_clone(m_packet);
      m_packet_depot.push_back(packet);
    }

    ::av_packet_unref(m_packet);

    return ec::none;
  }
  void _frame_loop()
  {
    using namespace std::literals::chrono_literals;
    while (true)
    {
      std::unique_lock<std::mutex> locker(m_mu);
      if (m_cv.wait_for(locker, 1ms, [&] { return m_need_to_stop; }))
      {
        break;
      }
      locker.unlock();

      _read_frame();
    }
  }
  int _read_frame()
  {
    int ret = 0;

    ret = _get_video_frame();
    if (ret < 0)
    {
      on_error(ret, "get_video_frame");
      return ec::rtsp_getting_frame_is_failed;
    }

    auto duration = (m_frame_rate.num && m_frame_rate.den ? ::av_q2d((AVRational) { m_frame_rate.den, m_frame_rate.num }) : 0);
    auto pts = (m_frame->pts == AV_NOPTS_VALUE) ? NAN : m_frame->pts * ::av_q2d(m_time_base);

    AVFrame* frame = ::av_frame_clone(m_frame);
    m_frame_depot.push_back(frame);
    ::av_frame_unref(m_frame);

    return ec::none;
  }
  int _get_video_frame()
  {
    int got_frame;

    got_frame = _decoder_decode_frame();
    if (got_frame < 0)
      return -1;

    if (got_frame)
    {
      double dpts = NAN;

      if (m_frame->pts != AV_NOPTS_VALUE)
      {
        dpts = ::av_q2d(m_video_stream->time_base) * m_frame->pts;
      }

      m_frame->sample_aspect_ratio = ::av_guess_sample_aspect_ratio(m_fctx, m_video_stream, m_frame);

      // TODO: frame drop
    }

    return got_frame;
  }
  int _decoder_decode_frame()
  {
    int ret = AVERROR(EAGAIN);

    while (true)
    {


    }

  }
  void on_error(int ec, const char* where)
  {
    std::stringstream ss;
    ss << where << "::error[" << ec << "].\n";
    std::cerr << ss.str() << std::flush;
  }
};

client::client()
  : impl(new implement(this))
{}
client::~client()
{
  if (impl)
  {
    delete impl;
    impl = nullptr;
  }
}
int client::open(const char* _url)
{
  return impl->open(_url);
}
int client::close()
{
  return impl->close();
}

} // namespace rtsp