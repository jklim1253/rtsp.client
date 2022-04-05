#ifndef __RTSP_CLIENT_HPP__
#define __RTSP_CLIENT_HPP__

namespace rtsp
{

struct client
{
  client();
  ~client();

  int open(const char* _url);
  int close();

private:
  struct implement;
  implement* impl;
};

} // namespace rtsp

#endif // __RTSP_CLIENT_HPP__