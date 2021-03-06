/*
* ipop-project
* Copyright 2016, University of Florida
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/

#if defined (_IPOP_LINUX)
#include "tapdev_lnx.h"
#include "tincan_exception.h"
#include <sys/types.h>
#include <sys/socket.h>

extern "C"
{
  #include "iproute2/utils.h"
  #include "iproute2/ip_common.h"
  #include "iproute2/ll_map.h"
}

namespace tincan
{
namespace linux
{

static const char * const TUN_PATH = "/dev/net/tun";

TapDevLnx::TapDevLnx() :
  fd_(-1),
  is_good_(false)
{
  memset(&ifr_, 0x0, sizeof(ifr_));
}

TapDevLnx::~TapDevLnx()
{
  if (fd_ > -1)
  close(fd_);
}

void TapDevLnx::Open(
  const TapDescriptor & tap_desc)
{
  string emsg("The Tap device open operation failed - ");
  //const char* tap_name = tap_desc.name;
  if((fd_ = open(TUN_PATH, O_RDWR)) < 0)
    throw TCEXCEPT(emsg.c_str());
  ifr_.ifr_flags = IFF_TAP | IFF_NO_PI;
  if(tap_desc.name.length() >= IFNAMSIZ)
  {
    emsg.append("the name length is longer than maximum allowed.");
    throw TCEXCEPT(emsg.c_str());
  }
  strncpy(ifr_.ifr_name, tap_desc.name.c_str(), tap_desc.name.length());
  ifr_.ifr_name[tap_desc.name.length()] = 0;
  //create the device
  if(ioctl(fd_, TUNSETIFF, (void *)&ifr_) < 0)
  {
    emsg.append("the device could not be created.");
    throw TCEXCEPT(emsg.c_str());
  }
  int cfg_skt;
  if((cfg_skt = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
  {
    emsg.append("a socket bind failed.");
    throw TCEXCEPT(emsg.c_str());
  }

  if((ioctl(cfg_skt, SIOCGIFHWADDR, &ifr_)) < 0)
  {
    emsg.append("retrieving the device mac address failed");
    close(cfg_skt);
    throw TCEXCEPT(emsg.c_str());
  }
  memcpy(mac_.data(), ifr_.ifr_hwaddr.sa_data, 6);
  close(cfg_skt);
}

int TapDevLnx::DeleteTapDevice(
  const string& TapName)
{
  struct rtnl_handle rth = { .fd = -1 };
  if (rtnl_open(&rth, 0) < 0)
  {
    LOG(LS_ERROR) << "Failed to open the net link device";
    return -1;
  }

  const char* dev = TapName.c_str();;
  iplink_req req;
  memset(&req, 0, sizeof(req));

  req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
  req.n.nlmsg_flags = NLM_F_REQUEST;
  req.n.nlmsg_type = RTM_DELLINK;
  req.i.ifi_family = AF_PACKET;

  ll_init_map(&rth);
  req.i.ifi_index = ll_name_to_index(dev);
  if (req.i.ifi_index == 0) {
    LOG(LS_ERROR) << "Cannot enumerate TAP device " << dev;
    rtnl_close(&rth);
    return -1;
  }
  if (rtnl_talk(&rth, &req.n, NULL) < 0)
  {
    LOG(LS_ERROR) << "Failed to send NetLink msg to TAP device";
    rtnl_close(&rth);
    return -1;
  }
  rtnl_close(&rth);
  return 0;
}

void TapDevLnx::Close()
{
  close(fd_);
  fd_ = -1;
  if (DeleteTapDevice(ifr_.ifr_name) < 0)
  {
    string emsg = "The TAP delete operation failed. ";
    LOG(LS_ERROR) << emsg << "errorno (" << errno << ") - " << strerror(errno);
    //throw TCEXCEPT(emsg.c_str());
  }
  LOG(LS_INFO) << "TAP device successfully removed";
}

void TapDevLnx::PlenToIpv4Mask(
  unsigned int prefix_len,
  struct sockaddr * writeback)
{
  uint32_t net_mask_int = ~(0u) << (32 - prefix_len);
  net_mask_int = htonl(net_mask_int);
  struct sockaddr_in netmask = {
        .sin_family = AF_INET,
        .sin_port = 0
  };
  struct in_addr netmask_addr = { .s_addr = net_mask_int };
  netmask.sin_addr = netmask_addr;
  //wrap sockaddr_in into sock_addr struct
  memcpy(writeback, &netmask, sizeof(struct sockaddr));
}


/**
 * Given some flags to enable and disable, reads the current flags for the
 * network device, and then ensures the high bits in enable are also high in
 * ifr_flags, and the high bits in disable are low in ifr_flags. The results are
 * then written back. For a list of valid flags, read the "SIOCGIFFLAGS,
 * SIOCSIFFLAGS" section of the 'man netdevice' page. You can pass `(short)0` if
 * you don't want to enable or disable any flags.
 */

void TapDevLnx::SetFlags(
  short enable,
  short disable)
{
  int cfg_skt;
  string emsg("The TAP device set flags operation failed");
  if ((cfg_skt = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
  {
    emsg.append("a socket bind failed.");
    throw TCEXCEPT(emsg.c_str());
  }

  if(ioctl(cfg_skt, SIOCGIFFLAGS, &ifr_) < 0)
  {
    close(cfg_skt);
    throw TCEXCEPT(emsg.c_str());
  }
  //set or unset the right flags
  ifr_.ifr_flags |= enable;
  ifr_.ifr_flags &= ~disable;
  //write back the modified flag states
  if (ioctl(cfg_skt, SIOCSIFFLAGS, &ifr_) < 0)
  {
    close(cfg_skt);
    throw TCEXCEPT(emsg.c_str());
  }
  close(cfg_skt);
}

uint32_t TapDevLnx::Read(AsyncIo& aio_rd)
{
  if(!is_good_ || reader_->IsQuitting())
    return 1; //indicates a failure to setup async operation
  TapMessageData *tp_ = new TapMessageData;
  tp_->aio_ = &aio_rd;
  reader_->Post(RTC_FROM_HERE, this, MSGID_READ, tp_);
  return 0;
}

uint32_t TapDevLnx::Write(AsyncIo& aio_wr)
{
  if(!is_good_ || writer_->IsQuitting())
    return 1; //indicates a failure to setup async operation
  TapMessageData *tp_ = new TapMessageData;
  tp_->aio_ = &aio_wr;
  writer_->Post(RTC_FROM_HERE, this, MSGID_WRITE, tp_);
  return 0;
}

uint16_t TapDevLnx::TapDevLnx::Mtu()
{
  return ifr_.ifr_mtu;
}

MacAddressType TapDevLnx::MacAddress()
{
  return mac_;
}

  /**
  * Sets and unsets some common flags used on a TAP device, namely, it sets the
  * IFF_NOARP flag, and unsets IFF_MULTICAST and IFF_BROADCAST. Notably, if
  * IFF_NOARP is not set, when using an IPv6 TAP, applications will have trouble
  * routing their data through the TAP device (Because they'd expect an ARP
  * response, which we aren't really willing to provide).
  */
void TapDevLnx::Up()
{
  is_good_ = true;
  SetFlags(IFF_UP | IFF_RUNNING, 0);
  if (reader_)
  {
    reader_->Quit();
    reader_.reset();
  }
  reader_ = make_unique<rtc::Thread>();
  reader_->Start();
  if (writer_)
  {
    writer_->Quit();
    writer_.reset();
  }
  writer_ = make_unique<rtc::Thread>();
  writer_->Start();
}

void TapDevLnx::Down()
{
  is_good_ = false;
  if(reader_)
    reader_->Quit();
  if(writer_)
    writer_->Quit();
  reader_.reset();
  writer_.reset();
  SetFlags(0, IFF_UP);
  LOG(LS_INFO) << "TAP device state set to DOWN";
}


void TapDevLnx::OnMessage(Message * msg)
{
  switch(msg->message_id)
  {
  case MSGID_READ:
  {
    AsyncIo* aio_read = ((TapMessageData*)msg->pdata)->aio_;
    if (is_good_) {
      int nread = read(fd_, aio_read->BufferToTransfer(), aio_read->BytesToTransfer());
      aio_read->good_ = (nread >= 0);
      aio_read->BytesTransferred(nread);
      read_completion_(aio_read);
    }
    else
    {
      LOG(LS_INFO) << "TAP shutting down, dropping IO.";
      delete aio_read;
    }
  }
  break;
  case MSGID_WRITE:
  {
    AsyncIo* aio_write = ((TapMessageData*)msg->pdata)->aio_;
    int nwrite = write(fd_, aio_write->BufferToTransfer(), aio_write->BytesToTransfer());
    if(nwrite < 0)
    {
      LOG(LS_WARNING) << "A TAP Write operation failed.";
      aio_write->good_ = false;
    }
    else
    {
      aio_write->good_ = true;
    }
    aio_write->BytesTransferred(nwrite);
    write_completion_(aio_write);
  }
  break;
  }
  delete (TapMessageData*)msg->pdata;
}

IP4AddressType
TapDevLnx::Ip4()
{
  return ip4_;
}
} // linux
} // tincan
#endif // _IPOP_LINUX
