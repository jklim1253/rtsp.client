#ifndef __RTSP_ERROR_HPP__
#define __RTSP_ERROR_HPP__

#include <cstdint>

namespace rtsp
{
namespace ec // error-code
{
enum : std::int32_t
{
  none = 0,
  not_implemented_yet,
  abnormal_arguments,

  // rtsp-relatives
  rtsp_error = 5000,
  rtsp_setting_option_is_failed,
  rtsp_opening_stream_is_failed,
  rtsp_finding_stream_info_is_failed,
  rtsp_starting_read_stream_is_failed,
  rtsp_reading_packet_is_failed,
  rtsp_video_stream_is_only,
  rtsp_memory_alloc_is_failed,
  rtsp_packet_seg,
  rtsp_codec_context_is_failed,
  rtsp_codec_open_is_failed,
  rtsp_getting_frame_is_failed,
};

} // namespace rtsp::ec
} // namespace rtsp

#endif // __RTSP_ERROR_HPP__