/*
 * dvbdevice.c: The DVB device interface
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: dvbdevice.c 2.21 2009/06/06 11:17:20 kls Exp $
 */

#include "dvbdevice.h"
#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/video.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "channels.h"
#include "diseqc.h"
#include "dvbci.h"
#include "dvbosd.h"
#include "eitscan.h"
#include "player.h"
#include "receiver.h"
#include "status.h"
#include "transfer.h"

#define DO_REC_AND_PLAY_ON_PRIMARY_DEVICE 1
#define DO_MULTIPLE_RECORDINGS 1

#define DEV_VIDEO         "/dev/video"
#define DEV_DVB_ADAPTER   "/dev/dvb/adapter"
#define DEV_DVB_OSD       "osd"
#define DEV_DVB_FRONTEND  "frontend"
#define DEV_DVB_DVR       "dvr"
#define DEV_DVB_DEMUX     "demux"
#define DEV_DVB_VIDEO     "video"
#define DEV_DVB_AUDIO     "audio"
#define DEV_DVB_CA        "ca"

#define DVBS_TUNE_TIMEOUT  9000 //ms
#define DVBS_LOCK_TIMEOUT  2000 //ms
#define DVBC_TUNE_TIMEOUT  9000 //ms
#define DVBC_LOCK_TIMEOUT  2000 //ms
#define DVBT_TUNE_TIMEOUT  9000 //ms
#define DVBT_LOCK_TIMEOUT  2000 //ms

class cDvbName {
private:
  char buffer[PATH_MAX];
public:
  cDvbName(const char *Name, int n) {
    snprintf(buffer, sizeof(buffer), "%s%d/%s%d", DEV_DVB_ADAPTER, n, Name, 0);
    }
  const char *operator*() { return buffer; }
  };

static int DvbOpen(const char *Name, int n, int Mode, bool ReportError = false)
{
  const char *FileName = *cDvbName(Name, n);
  int fd = open(FileName, Mode);
  if (fd < 0 && ReportError)
     LOG_ERROR_STR(FileName);
  return fd;
}

// --- cDvbTuner -------------------------------------------------------------

class cDvbTuner : public cThread {
private:
  enum eTunerStatus { tsIdle, tsSet, tsTuned, tsLocked };
  int fd_frontend;
  int cardIndex;
  int tuneTimeout;
  int lockTimeout;
  time_t lastTimeoutReport;
  fe_delivery_system frontendType;
  cChannel channel;
  const char *diseqcCommands;
  eTunerStatus tunerStatus;
  cMutex mutex;
  cCondVar locked;
  cCondVar newSet;
  bool GetFrontendStatus(fe_status_t &Status, int TimeoutMs = 0);
  bool SetFrontend(void);
  virtual void Action(void);
public:
  cDvbTuner(int Fd_Frontend, int CardIndex, fe_delivery_system FrontendType);
  virtual ~cDvbTuner();
  bool IsTunedTo(const cChannel *Channel) const;
  void Set(const cChannel *Channel, bool Tune);
  bool Locked(int TimeoutMs = 0);
  };

cDvbTuner::cDvbTuner(int Fd_Frontend, int CardIndex, fe_delivery_system FrontendType)
{
  fd_frontend = Fd_Frontend;
  cardIndex = CardIndex;
  frontendType = FrontendType;
  tuneTimeout = 0;
  lockTimeout = 0;
  lastTimeoutReport = 0;
  diseqcCommands = NULL;
  tunerStatus = tsIdle;
  if (frontendType == SYS_DVBS || frontendType == SYS_DVBS2)
     CHECK(ioctl(fd_frontend, FE_SET_VOLTAGE, SEC_VOLTAGE_13)); // must explicitly turn on LNB power
  SetDescription("tuner on device %d", cardIndex + 1);
  Start();
}

cDvbTuner::~cDvbTuner()
{
  tunerStatus = tsIdle;
  newSet.Broadcast();
  locked.Broadcast();
  Cancel(3);
}

bool cDvbTuner::IsTunedTo(const cChannel *Channel) const
{
  if (tunerStatus == tsIdle)
     return false; // not tuned to
  if (channel.Source() != Channel->Source() || channel.Transponder() != Channel->Transponder())
     return false; // sufficient mismatch
  char Type = **cSource::ToString(Channel->Source());
#define ST(s, p) if (strchr(s, Type)) if (channel.p() != Channel->p()) return false;
  // Polarization is already checked as part of the Transponder.
  ST("  T", Bandwidth);
  ST("CST", CoderateH);
  ST("  T", CoderateL);
  ST("  T", Guard);
  ST("CST", Inversion);
  ST("CST", Modulation);
  ST(" S ", RollOff);
  ST(" S ", System);
  ST("CS ", Srate);
  ST("  T", Transmission);
  ST("  T", Hierarchy);
  return true;
}

void cDvbTuner::Set(const cChannel *Channel, bool Tune)
{
  cMutexLock MutexLock(&mutex);
  if (Tune)
     tunerStatus = tsSet;
  channel = *Channel;
  lastTimeoutReport = 0;
  newSet.Broadcast();
}

bool cDvbTuner::Locked(int TimeoutMs)
{
  bool isLocked = (tunerStatus >= tsLocked);
  if (isLocked || !TimeoutMs)
     return isLocked;

  cMutexLock MutexLock(&mutex);
  if (TimeoutMs && tunerStatus < tsLocked)
     locked.TimedWait(mutex, TimeoutMs);
  return tunerStatus >= tsLocked;
}

bool cDvbTuner::GetFrontendStatus(fe_status_t &Status, int TimeoutMs)
{
  if (TimeoutMs) {
     cPoller Poller(fd_frontend);
     if (Poller.Poll(TimeoutMs)) {
        dvb_frontend_event Event;
        while (ioctl(fd_frontend, FE_GET_EVENT, &Event) == 0)
              ; // just to clear the event queue - we'll read the actual status below
        }
     }
  while (1) {
        if (ioctl(fd_frontend, FE_READ_STATUS, &Status) != -1)
           return true;
        if (errno != EINTR)
           break;
        }
  return false;
}

static unsigned int FrequencyToHz(unsigned int f)
{
  while (f && f < 1000000)
        f *= 1000;
  return f;
}

bool cDvbTuner::SetFrontend(void)
{
#define MAXFRONTENDCMDS 16
#define SETCMD(c, d) { Frontend[CmdSeq.num].cmd = (c);\
                       Frontend[CmdSeq.num].u.data = (d);\
                       if (CmdSeq.num++ > MAXFRONTENDCMDS) {\
                          esyslog("ERROR: too many tuning commands on frontend %d", cardIndex);\
                          return false;\
                          }\
                     }
  dtv_property Frontend[MAXFRONTENDCMDS];
  memset(&Frontend, 0, sizeof(Frontend));
  dtv_properties CmdSeq;
  memset(&CmdSeq, 0, sizeof(CmdSeq));
  CmdSeq.props = Frontend;
  SETCMD(DTV_CLEAR, 0);
  if (ioctl(fd_frontend, FE_SET_PROPERTY, &CmdSeq) < 0) {
     esyslog("ERROR: frontend %d: %m", cardIndex);
     return false;
     }
  CmdSeq.num = 0;

  if (frontendType == SYS_DVBS || frontendType == SYS_DVBS2) {
     unsigned int frequency = channel.Frequency();
     if (Setup.DiSEqC) {
        cDiseqc *diseqc = Diseqcs.Get(channel.Source(), channel.Frequency(), channel.Polarization());
        if (diseqc) {
           if (diseqc->Commands() && (!diseqcCommands || strcmp(diseqcCommands, diseqc->Commands()) != 0)) {
              cDiseqc::eDiseqcActions da;
              for (char *CurrentAction = NULL; (da = diseqc->Execute(&CurrentAction)) != cDiseqc::daNone; ) {
                  switch (da) {
                    case cDiseqc::daNone:      break;
                    case cDiseqc::daToneOff:   CHECK(ioctl(fd_frontend, FE_SET_TONE, SEC_TONE_OFF)); break;
                    case cDiseqc::daToneOn:    CHECK(ioctl(fd_frontend, FE_SET_TONE, SEC_TONE_ON)); break;
                    case cDiseqc::daVoltage13: CHECK(ioctl(fd_frontend, FE_SET_VOLTAGE, SEC_VOLTAGE_13)); break;
                    case cDiseqc::daVoltage18: CHECK(ioctl(fd_frontend, FE_SET_VOLTAGE, SEC_VOLTAGE_18)); break;
                    case cDiseqc::daMiniA:     CHECK(ioctl(fd_frontend, FE_DISEQC_SEND_BURST, SEC_MINI_A)); break;
                    case cDiseqc::daMiniB:     CHECK(ioctl(fd_frontend, FE_DISEQC_SEND_BURST, SEC_MINI_B)); break;
                    case cDiseqc::daCodes: {
                         int n = 0;
                         uchar *codes = diseqc->Codes(n);
                         if (codes) {
                            struct dvb_diseqc_master_cmd cmd;
                            cmd.msg_len = min(n, int(sizeof(cmd.msg)));
                            memcpy(cmd.msg, codes, cmd.msg_len);
                            CHECK(ioctl(fd_frontend, FE_DISEQC_SEND_MASTER_CMD, &cmd));
                            }
                         }
                         break;
                    }
                  }
              diseqcCommands = diseqc->Commands();
              }
           frequency -= diseqc->Lof();
           }
        else {
           esyslog("ERROR: no DiSEqC parameters found for channel %d", channel.Number());
           return false;
           }
        }
     else {
        int tone = SEC_TONE_OFF;
        if (frequency < (unsigned int)Setup.LnbSLOF) {
           frequency -= Setup.LnbFrequLo;
           tone = SEC_TONE_OFF;
           }
        else {
           frequency -= Setup.LnbFrequHi;
           tone = SEC_TONE_ON;
           }
        int volt = (channel.Polarization() == 'v' || channel.Polarization() == 'V' || channel.Polarization() == 'r' || channel.Polarization() == 'R') ? SEC_VOLTAGE_13 : SEC_VOLTAGE_18;
        CHECK(ioctl(fd_frontend, FE_SET_VOLTAGE, volt));
        CHECK(ioctl(fd_frontend, FE_SET_TONE, tone));
        }
     frequency = abs(frequency); // Allow for C-band, where the frequency is less than the LOF

     // DVB-S/DVB-S2 (common parts)
     SETCMD(DTV_DELIVERY_SYSTEM, channel.System());
     SETCMD(DTV_FREQUENCY, frequency * 1000UL);
     SETCMD(DTV_MODULATION, channel.Modulation());
     SETCMD(DTV_SYMBOL_RATE, channel.Srate() * 1000UL);
     SETCMD(DTV_INNER_FEC, channel.CoderateH());
     SETCMD(DTV_INVERSION, channel.Inversion());
     if (channel.System() == SYS_DVBS2) {
        if (frontendType == SYS_DVBS2) {
           // DVB-S2
           SETCMD(DTV_PILOT, PILOT_AUTO);
           SETCMD(DTV_ROLLOFF, channel.RollOff());
           }
        else {
           esyslog("ERROR: frontend %d doesn't provide DVB-S2", cardIndex);
           return false;
           }
        }
     else {
        // DVB-S
        SETCMD(DTV_ROLLOFF, ROLLOFF_35); // DVB-S always has a ROLLOFF of 0.35
        }

     tuneTimeout = DVBS_TUNE_TIMEOUT;
     lockTimeout = DVBS_LOCK_TIMEOUT;
     }
  else if (frontendType == SYS_DVBC_ANNEX_AC || frontendType == SYS_DVBC_ANNEX_B) {
     // DVB-C
     SETCMD(DTV_DELIVERY_SYSTEM, frontendType);
     SETCMD(DTV_FREQUENCY, FrequencyToHz(channel.Frequency()));
     SETCMD(DTV_INVERSION, channel.Inversion());
     SETCMD(DTV_SYMBOL_RATE, channel.Srate() * 1000UL);
     SETCMD(DTV_INNER_FEC, channel.CoderateH());
     SETCMD(DTV_MODULATION, channel.Modulation());

     tuneTimeout = DVBC_TUNE_TIMEOUT;
     lockTimeout = DVBC_LOCK_TIMEOUT;
     }
  else if (frontendType == SYS_DVBT) {
     // DVB-T
     SETCMD(DTV_DELIVERY_SYSTEM, frontendType);
     SETCMD(DTV_FREQUENCY, FrequencyToHz(channel.Frequency()));
     SETCMD(DTV_INVERSION, channel.Inversion());
     SETCMD(DTV_BANDWIDTH_HZ, channel.Bandwidth());
     SETCMD(DTV_CODE_RATE_HP, channel.CoderateH());
     SETCMD(DTV_CODE_RATE_LP, channel.CoderateL());
     SETCMD(DTV_MODULATION, channel.Modulation());
     SETCMD(DTV_TRANSMISSION_MODE, channel.Transmission());
     SETCMD(DTV_GUARD_INTERVAL, channel.Guard());
     SETCMD(DTV_HIERARCHY, channel.Hierarchy());

     tuneTimeout = DVBT_TUNE_TIMEOUT;
     lockTimeout = DVBT_LOCK_TIMEOUT;
     }
  else {
     esyslog("ERROR: attempt to set channel with unknown DVB frontend type");
     return false;
     }
  SETCMD(DTV_TUNE, 0);
  if (ioctl(fd_frontend, FE_SET_PROPERTY, &CmdSeq) < 0) {
     esyslog("ERROR: frontend %d: %m", cardIndex);
     return false;
     }
  return true;
}

void cDvbTuner::Action(void)
{
  cTimeMs Timer;
  bool LostLock = false;
  fe_status_t Status = (fe_status_t)0;
  while (Running()) {
        fe_status_t NewStatus;
        if (GetFrontendStatus(NewStatus, 10))
           Status = NewStatus;
        cMutexLock MutexLock(&mutex);
        switch (tunerStatus) {
          case tsIdle:
               break;
          case tsSet:
               tunerStatus = SetFrontend() ? tsTuned : tsIdle;
               Timer.Set(tuneTimeout);
               continue;
          case tsTuned:
               if (Timer.TimedOut()) {
                  tunerStatus = tsSet;
                  diseqcCommands = NULL;
                  if (time(NULL) - lastTimeoutReport > 60) { // let's not get too many of these
                     isyslog("frontend %d timed out while tuning to channel %d, tp %d", cardIndex, channel.Number(), channel.Transponder());
                     lastTimeoutReport = time(NULL);
                     }
                  continue;
                  }
          case tsLocked:
               if (Status & FE_REINIT) {
                  tunerStatus = tsSet;
                  diseqcCommands = NULL;
                  isyslog("frontend %d was reinitialized", cardIndex);
                  lastTimeoutReport = 0;
                  continue;
                  }
               else if (Status & FE_HAS_LOCK) {
                  if (LostLock) {
                     isyslog("frontend %d regained lock on channel %d, tp %d", cardIndex, channel.Number(), channel.Transponder());
                     LostLock = false;
                     }
                  tunerStatus = tsLocked;
                  locked.Broadcast();
                  lastTimeoutReport = 0;
                  }
               else if (tunerStatus == tsLocked) {
                  LostLock = true;
                  isyslog("frontend %d lost lock on channel %d, tp %d", cardIndex, channel.Number(), channel.Transponder());
                  tunerStatus = tsTuned;
                  Timer.Set(lockTimeout);
                  lastTimeoutReport = 0;
                  continue;
                  }
          }

        if (tunerStatus != tsTuned)
           newSet.TimedWait(mutex, 1000);
        }
}

// --- cDvbDevice ------------------------------------------------------------

int cDvbDevice::devVideoOffset = -1;
int cDvbDevice::setTransferModeForDolbyDigital = 1;

const char *DeliverySystems[] = {
  "UNDEFINED",
  "DVB-C",
  "DVB-C",
  "DVB-T",
  "DSS",
  "DVB-S",
  "DVB-S2",
  "DVB-H",
  "ISDBT",
  "ISDBS",
  "ISDBC",
  "ATSC",
  "ATSCMH",
  "DMBTH",
  "CMMB",
  "DAB",
  NULL
  };

cDvbDevice::cDvbDevice(int n)
{
  ciAdapter = NULL;
  dvbTuner = NULL;
  frontendType = SYS_UNDEFINED;
  numProvidedSystems = 0;
  spuDecoder = NULL;
  digitalAudio = false;
  playMode = pmNone;

  // Devices that are present on all card types:

  int fd_frontend = DvbOpen(DEV_DVB_FRONTEND, n, O_RDWR | O_NONBLOCK);

  // Devices that are only present on cards with decoders:

  fd_osd      = DvbOpen(DEV_DVB_OSD,    n, O_RDWR);
  fd_video    = DvbOpen(DEV_DVB_VIDEO,  n, O_RDWR | O_NONBLOCK);
  fd_audio    = DvbOpen(DEV_DVB_AUDIO,  n, O_RDWR | O_NONBLOCK);
  fd_stc      = DvbOpen(DEV_DVB_DEMUX,  n, O_RDWR);

  // Common Interface:

  fd_ca       = DvbOpen(DEV_DVB_CA,     n, O_RDWR);
  if (fd_ca >= 0)
     ciAdapter = cDvbCiAdapter::CreateCiAdapter(this, fd_ca);

  // The DVR device (will be opened and closed as needed):

  fd_dvr = -1;

  // The offset of the /dev/video devices:

  if (devVideoOffset < 0) { // the first one checks this
     FILE *f = NULL;
     char buffer[PATH_MAX];
     for (int ofs = 0; ofs < 100; ofs++) {
         snprintf(buffer, sizeof(buffer), "/proc/video/dev/video%d", ofs);
         if ((f = fopen(buffer, "r")) != NULL) {
            if (fgets(buffer, sizeof(buffer), f)) {
               if (strstr(buffer, "DVB Board")) { // found the _first_ DVB card
                  devVideoOffset = ofs;
                  dsyslog("video device offset is %d", devVideoOffset);
                  break;
                  }
               }
            else
               break;
            fclose(f);
            }
         else
            break;
         }
     if (devVideoOffset < 0)
        devVideoOffset = 0;
     if (f)
        fclose(f);
     }
  devVideoIndex = (devVideoOffset >= 0 && HasDecoder()) ? devVideoOffset++ : -1;

  // Video format:

  SetVideoFormat(Setup.VideoFormat);

  // We only check the devices that must be present - the others will be checked before accessing them://XXX

  if (fd_frontend >= 0) {
     if (ioctl(fd_frontend, FE_GET_INFO, &frontendInfo) >= 0) {
        switch (frontendInfo.type) {
          case FE_QPSK: frontendType = (frontendInfo.caps & FE_CAN_2G_MODULATION) ? SYS_DVBS2 : SYS_DVBS; break;
          case FE_OFDM: frontendType = SYS_DVBT; break;
          case FE_QAM:  frontendType = SYS_DVBC_ANNEX_AC; break;
          case FE_ATSC: frontendType = SYS_ATSC; break;
          default: esyslog("ERROR: unknown frontend type %d on device %d", frontendInfo.type, CardIndex() + 1);
          }
        }
     else
        LOG_ERROR;
     if (frontendType != SYS_UNDEFINED) {
        numProvidedSystems++;
        if (frontendType == SYS_DVBS2)
           numProvidedSystems++;
        isyslog("device %d provides %s (\"%s\")", CardIndex() + 1, DeliverySystems[frontendType], frontendInfo.name);
        dvbTuner = new cDvbTuner(fd_frontend, CardIndex(), frontendType);
        }
     }
  else
     esyslog("ERROR: can't open DVB device %d", n);

  StartSectionHandler();
}

cDvbDevice::~cDvbDevice()
{
  StopSectionHandler();
  delete spuDecoder;
  delete dvbTuner;
  delete ciAdapter;
  // We're not explicitly closing any device files here, since this sometimes
  // caused segfaults. Besides, the program is about to terminate anyway...
}

bool cDvbDevice::Probe(const char *FileName)
{
  if (access(FileName, F_OK) == 0) {
     dsyslog("probing %s", FileName);
     int f = open(FileName, O_RDONLY);
     if (f >= 0) {
        close(f);
        return true;
        }
     else if (errno != ENODEV && errno != EINVAL)
        LOG_ERROR_STR(FileName);
     }
  else if (errno != ENOENT)
     LOG_ERROR_STR(FileName);
  return false;
}

bool cDvbDevice::Initialize(void)
{
  int found = 0;
  int i;
  for (i = 0; i < MAXDVBDEVICES; i++) {
      if (UseDevice(NextCardIndex())) {
         if (Probe(*cDvbName(DEV_DVB_FRONTEND, i))) {
            new cDvbDevice(i);
            found++;
            }
         else
            break;
         }
      else
         NextCardIndex(1); // skips this one
      }
  NextCardIndex(MAXDVBDEVICES - i); // skips the rest
  if (found > 0)
     isyslog("found %d video device%s", found, found > 1 ? "s" : "");
  else
     isyslog("no DVB device found");
  return found > 0;
}

void cDvbDevice::MakePrimaryDevice(bool On)
{
  if (On && HasDecoder())
     new cDvbOsdProvider(fd_osd);
}

bool cDvbDevice::HasDecoder(void) const
{
  return fd_video >= 0 && fd_audio >= 0;
}

bool cDvbDevice::Ready(void)
{
  if (ciAdapter)
     return ciAdapter->Ready();
  return true;
}

cSpuDecoder *cDvbDevice::GetSpuDecoder(void)
{
  if (!spuDecoder && IsPrimaryDevice())
     spuDecoder = new cDvbSpuDecoder();
  return spuDecoder;
}

bool cDvbDevice::HasCi(void)
{
  return ciAdapter;
}

uchar *cDvbDevice::GrabImage(int &Size, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  if (devVideoIndex < 0)
     return NULL;
  char buffer[PATH_MAX];
  snprintf(buffer, sizeof(buffer), "%s%d", DEV_VIDEO, devVideoIndex);
  int videoDev = open(buffer, O_RDWR);
  if (videoDev >= 0) {
     uchar *result = NULL;
     // set up the size and RGB
     v4l2_format fmt;
     memset(&fmt, 0, sizeof(fmt));
     fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
     fmt.fmt.pix.width = SizeX;
     fmt.fmt.pix.height = SizeY;
     fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
     fmt.fmt.pix.field = V4L2_FIELD_ANY;
     if (ioctl(videoDev, VIDIOC_S_FMT, &fmt) == 0) {
        v4l2_requestbuffers reqBuf;
        memset(&reqBuf, 0, sizeof(reqBuf));
        reqBuf.count = 2;
        reqBuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        reqBuf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(videoDev, VIDIOC_REQBUFS, &reqBuf) >= 0) {
           v4l2_buffer mbuf;
           memset(&mbuf, 0, sizeof(mbuf));
           mbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
           mbuf.memory = V4L2_MEMORY_MMAP;
           if (ioctl(videoDev, VIDIOC_QUERYBUF, &mbuf) == 0) {
              int msize = mbuf.length;
              unsigned char *mem = (unsigned char *)mmap(0, msize, PROT_READ | PROT_WRITE, MAP_SHARED, videoDev, 0);
              if (mem && mem != (unsigned char *)-1) {
                 v4l2_buffer buf;
                 memset(&buf, 0, sizeof(buf));
                 buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                 buf.memory = V4L2_MEMORY_MMAP;
                 buf.index = 0;
                 if (ioctl(videoDev, VIDIOC_QBUF, &buf) == 0) {
                    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    if (ioctl (videoDev, VIDIOC_STREAMON, &type) == 0) {
                       memset(&buf, 0, sizeof(buf));
                       buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                       buf.memory = V4L2_MEMORY_MMAP;
                       buf.index = 0;
                       if (ioctl(videoDev, VIDIOC_DQBUF, &buf) == 0) {
                          if (ioctl(videoDev, VIDIOC_STREAMOFF, &type) == 0) {
                             // make RGB out of BGR:
                             int memsize = fmt.fmt.pix.width * fmt.fmt.pix.height;
                             unsigned char *mem1 = mem;
                             for (int i = 0; i < memsize; i++) {
                                 unsigned char tmp = mem1[2];
                                 mem1[2] = mem1[0];
                                 mem1[0] = tmp;
                                 mem1 += 3;
                                 }

                             if (Quality < 0)
                                Quality = 100;

                             dsyslog("grabbing to %s %d %d %d", Jpeg ? "JPEG" : "PNM", Quality, fmt.fmt.pix.width, fmt.fmt.pix.height);
                             if (Jpeg) {
                                // convert to JPEG:
                                result = RgbToJpeg(mem, fmt.fmt.pix.width, fmt.fmt.pix.height, Size, Quality);
                                if (!result)
                                   esyslog("ERROR: failed to convert image to JPEG");
                                }
                             else {
                                // convert to PNM:
                                char buf[32];
                                snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
                                int l = strlen(buf);
                                int bytes = memsize * 3;
                                Size = l + bytes;
                                result = MALLOC(uchar, Size);
                                if (result) {
                                   memcpy(result, buf, l);
                                   memcpy(result + l, mem, bytes);
                                   }
                                else
                                   esyslog("ERROR: failed to convert image to PNM");
                                }
                             }
                          else
                             esyslog("ERROR: video device VIDIOC_STREAMOFF failed");
                          }
                       else
                          esyslog("ERROR: video device VIDIOC_DQBUF failed");
                       }
                    else
                       esyslog("ERROR: video device VIDIOC_STREAMON failed");
                    }
                 else
                    esyslog("ERROR: video device VIDIOC_QBUF failed");
                 munmap(mem, msize);
                 }
              else
                 esyslog("ERROR: failed to memmap video device");
              }
           else
              esyslog("ERROR: video device VIDIOC_QUERYBUF failed");
           }
        else
           esyslog("ERROR: video device VIDIOC_REQBUFS failed");
        }
     else
        esyslog("ERROR: video device VIDIOC_S_FMT failed");
     close(videoDev);
     return result;
     }
  else
     LOG_ERROR_STR(buffer);
  return NULL;
}

void cDvbDevice::SetVideoDisplayFormat(eVideoDisplayFormat VideoDisplayFormat)
{
  cDevice::SetVideoDisplayFormat(VideoDisplayFormat);
  if (HasDecoder()) {
     if (Setup.VideoFormat) {
        CHECK(ioctl(fd_video, VIDEO_SET_DISPLAY_FORMAT, VIDEO_LETTER_BOX));
        }
     else {
        switch (VideoDisplayFormat) {
          case vdfPanAndScan:
               CHECK(ioctl(fd_video, VIDEO_SET_DISPLAY_FORMAT, VIDEO_PAN_SCAN));
               break;
          case vdfLetterBox:
               CHECK(ioctl(fd_video, VIDEO_SET_DISPLAY_FORMAT, VIDEO_LETTER_BOX));
               break;
          case vdfCenterCutOut:
               CHECK(ioctl(fd_video, VIDEO_SET_DISPLAY_FORMAT, VIDEO_CENTER_CUT_OUT));
               break;
          }
        }
     }
}

void cDvbDevice::SetVideoFormat(bool VideoFormat16_9)
{
  if (HasDecoder()) {
     CHECK(ioctl(fd_video, VIDEO_SET_FORMAT, VideoFormat16_9 ? VIDEO_FORMAT_16_9 : VIDEO_FORMAT_4_3));
     SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
     }
}

eVideoSystem cDvbDevice::GetVideoSystem(void)
{
  eVideoSystem VideoSystem = vsPAL;
  if (fd_video >= 0) {
     video_size_t vs;
     if (ioctl(fd_video, VIDEO_GET_SIZE, &vs) == 0) {
        if (vs.h == 480 || vs.h == 240)
           VideoSystem = vsNTSC;
        }
     else
        LOG_ERROR;
     }
  return VideoSystem;
}

void cDvbDevice::GetVideoSize(int &Width, int &Height, double &VideoAspect)
{
  if (fd_video >= 0) {
     video_size_t vs;
     if (ioctl(fd_video, VIDEO_GET_SIZE, &vs) == 0) {
        Width = vs.w;
        Height = vs.h;
        switch (vs.aspect_ratio) {
          default:
          case VIDEO_FORMAT_4_3:   VideoAspect =  4.0 / 3.0; break;
          case VIDEO_FORMAT_16_9:  VideoAspect = 16.0 / 9.0; break;
          case VIDEO_FORMAT_221_1: VideoAspect =       2.21; break;
          }
        return;
        }
     else
        LOG_ERROR;
     }
  cDevice::GetVideoSize(Width, Height, VideoAspect);
}

void cDvbDevice::GetOsdSize(int &Width, int &Height, double &PixelAspect)
{
  if (fd_video >= 0) {
     video_size_t vs;
     if (ioctl(fd_video, VIDEO_GET_SIZE, &vs) == 0) {
        Width = 720;
        if (vs.h != 480 && vs.h != 240)
           Height = 576; // PAL
        else
           Height = 480; // NTSC
        switch (Setup.VideoFormat ? vs.aspect_ratio : VIDEO_FORMAT_4_3) {
          default:
          case VIDEO_FORMAT_4_3:   PixelAspect =  4.0 / 3.0; break;
          case VIDEO_FORMAT_221_1: // FF DVB cards only distinguish between 4:3 and 16:9
          case VIDEO_FORMAT_16_9:  PixelAspect = 16.0 / 9.0; break;
          }
        PixelAspect /= double(Width) / Height;
        return;
        }
     else
        LOG_ERROR;
     }
  cDevice::GetOsdSize(Width, Height, PixelAspect);
}

bool cDvbDevice::SetAudioBypass(bool On)
{
  if (setTransferModeForDolbyDigital != 1)
     return false;
  return ioctl(fd_audio, AUDIO_SET_BYPASS_MODE, On) == 0;
}

//                            ptAudio        ptVideo        ptPcr        ptTeletext        ptDolby        ptOther
dmx_pes_type_t PesTypes[] = { DMX_PES_AUDIO, DMX_PES_VIDEO, DMX_PES_PCR, DMX_PES_TELETEXT, DMX_PES_OTHER, DMX_PES_OTHER };

bool cDvbDevice::SetPid(cPidHandle *Handle, int Type, bool On)
{
  if (Handle->pid) {
     dmx_pes_filter_params pesFilterParams;
     memset(&pesFilterParams, 0, sizeof(pesFilterParams));
     if (On) {
        if (Handle->handle < 0) {
           Handle->handle = DvbOpen(DEV_DVB_DEMUX, CardIndex(), O_RDWR | O_NONBLOCK, true);
           if (Handle->handle < 0) {
              LOG_ERROR;
              return false;
              }
           }
        pesFilterParams.pid     = Handle->pid;
        pesFilterParams.input   = DMX_IN_FRONTEND;
        pesFilterParams.output  = (Type <= ptTeletext && Handle->used <= 1) ? DMX_OUT_DECODER : DMX_OUT_TS_TAP;
        pesFilterParams.pes_type= PesTypes[Type < ptOther ? Type : ptOther];
        pesFilterParams.flags   = DMX_IMMEDIATE_START;
        if (ioctl(Handle->handle, DMX_SET_PES_FILTER, &pesFilterParams) < 0) {
           LOG_ERROR;
           return false;
           }
        }
     else if (!Handle->used) {
        CHECK(ioctl(Handle->handle, DMX_STOP));
        if (Type <= ptTeletext) {
           pesFilterParams.pid     = 0x1FFF;
           pesFilterParams.input   = DMX_IN_FRONTEND;
           pesFilterParams.output  = DMX_OUT_DECODER;
           pesFilterParams.pes_type= PesTypes[Type];
           pesFilterParams.flags   = DMX_IMMEDIATE_START;
           CHECK(ioctl(Handle->handle, DMX_SET_PES_FILTER, &pesFilterParams));
           if (PesTypes[Type] == DMX_PES_VIDEO) // let's only do this once
              SetPlayMode(pmNone); // necessary to switch a PID from DMX_PES_VIDEO/AUDIO to DMX_PES_OTHER
           }
        close(Handle->handle);
        Handle->handle = -1;
        }
     }
  return true;
}

int cDvbDevice::OpenFilter(u_short Pid, u_char Tid, u_char Mask)
{
  const char *FileName = *cDvbName(DEV_DVB_DEMUX, CardIndex());
  int f = open(FileName, O_RDWR | O_NONBLOCK);
  if (f >= 0) {
     dmx_sct_filter_params sctFilterParams;
     memset(&sctFilterParams, 0, sizeof(sctFilterParams));
     sctFilterParams.pid = Pid;
     sctFilterParams.timeout = 0;
     sctFilterParams.flags = DMX_IMMEDIATE_START;
     sctFilterParams.filter.filter[0] = Tid;
     sctFilterParams.filter.mask[0] = Mask;
     if (ioctl(f, DMX_SET_FILTER, &sctFilterParams) >= 0)
        return f;
     else {
        esyslog("ERROR: can't set filter (pid=%d, tid=%02X, mask=%02X): %m", Pid, Tid, Mask);
        close(f);
        }
     }
  else
     esyslog("ERROR: can't open filter handle on '%s'", FileName);
  return -1;
}

void cDvbDevice::CloseFilter(int Handle)
{
  close(Handle);
}

void cDvbDevice::TurnOffLiveMode(bool LiveView)
{
  if (LiveView) {
     // Avoid noise while switching:
     CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, true));
     CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
     CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
     CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
     }

  // Turn off live PIDs:

  DetachAll(pidHandles[ptAudio].pid);
  DetachAll(pidHandles[ptVideo].pid);
  DetachAll(pidHandles[ptPcr].pid);
  DetachAll(pidHandles[ptTeletext].pid);
  DelPid(pidHandles[ptAudio].pid);
  DelPid(pidHandles[ptVideo].pid);
  DelPid(pidHandles[ptPcr].pid, ptPcr);
  DelPid(pidHandles[ptTeletext].pid);
  DelPid(pidHandles[ptDolby].pid);
}

bool cDvbDevice::ProvidesSource(int Source) const
{
  int type = Source & cSource::st_Mask;
  return type == cSource::stNone
      || type == cSource::stCable && (frontendType == SYS_DVBC_ANNEX_AC || frontendType == SYS_DVBC_ANNEX_B)
      || type == cSource::stSat   && (frontendType == SYS_DVBS || frontendType == SYS_DVBS2)
      || type == cSource::stTerr  && (frontendType == SYS_DVBT);
}

bool cDvbDevice::ProvidesTransponder(const cChannel *Channel) const
{
  if (!ProvidesSource(Channel->Source()))
     return false; // doesn't provide source
  if (!cSource::IsSat(Channel->Source()))
     return true; // source is sufficient for non sat
  if (frontendType == SYS_DVBS && Channel->System() == SYS_DVBS2)
     return false; // requires modulation system which frontend doesn't provide
  return !Setup.DiSEqC || Diseqcs.Get(Channel->Source(), Channel->Frequency(), Channel->Polarization());
}

bool cDvbDevice::ProvidesChannel(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers) const
{
  bool result = false;
  bool hasPriority = Priority < 0 || Priority > this->Priority();
  bool needsDetachReceivers = false;

  if (ProvidesTransponder(Channel)) {
     result = hasPriority;
     if (Priority >= 0 && Receiving(true)) {
        if (dvbTuner->IsTunedTo(Channel)) {
           if (Channel->Vpid() && !HasPid(Channel->Vpid()) || Channel->Apid(0) && !HasPid(Channel->Apid(0))) {
#ifdef DO_MULTIPLE_RECORDINGS
              if (CamSlot() && Channel->Ca() >= CA_ENCRYPTED_MIN) {
                 if (CamSlot()->CanDecrypt(Channel))
                    result = true;
                 else
                    needsDetachReceivers = true;
                 }
              else if (!IsPrimaryDevice())
                 result = true;
#ifdef DO_REC_AND_PLAY_ON_PRIMARY_DEVICE
              else
                 result = Priority >= Setup.PrimaryLimit;
#endif
#endif
              }
           else
              result = !IsPrimaryDevice() || Priority >= Setup.PrimaryLimit;
           }
        else
           needsDetachReceivers = true;
        }
     }
  if (NeedsDetachReceivers)
     *NeedsDetachReceivers = needsDetachReceivers;
  return result;
}

int cDvbDevice::NumProvidedSystems(void) const
{
  return numProvidedSystems;
}

bool cDvbDevice::IsTunedToTransponder(const cChannel *Channel)
{
  return dvbTuner->IsTunedTo(Channel);
}

bool cDvbDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
  int apid = Channel->Apid(0);
  int vpid = Channel->Vpid();
  int dpid = Channel->Dpid(0);

  bool DoTune = !dvbTuner->IsTunedTo(Channel);

  bool pidHandlesVideo = pidHandles[ptVideo].pid == vpid;
  bool pidHandlesAudio = pidHandles[ptAudio].pid == apid;

  bool TurnOffLivePIDs = HasDecoder()
                         && (DoTune
                            || !IsPrimaryDevice()
                            || LiveView // for a new live view the old PIDs need to be turned off
                            || pidHandlesVideo // for recording the PIDs must be shifted from DMX_PES_AUDIO/VIDEO to DMX_PES_OTHER
                            );

  bool StartTransferMode = IsPrimaryDevice() && !DoTune
                           && (LiveView && HasPid(vpid ? vpid : apid) && (!pidHandlesVideo || (!pidHandlesAudio && (dpid ? pidHandles[ptAudio].pid != dpid : true)))// the PID is already set as DMX_PES_OTHER
                              || !LiveView && (pidHandlesVideo || pidHandlesAudio) // a recording is going to shift the PIDs from DMX_PES_AUDIO/VIDEO to DMX_PES_OTHER
                              );
  if (CamSlot() && !ChannelCamRelations.CamDecrypt(Channel->GetChannelID(), CamSlot()->SlotNumber()))
     StartTransferMode |= LiveView && IsPrimaryDevice() && Channel->Ca() >= CA_ENCRYPTED_MIN;

  bool TurnOnLivePIDs = HasDecoder() && !StartTransferMode && LiveView;

#ifndef DO_MULTIPLE_RECORDINGS
  TurnOffLivePIDs = TurnOnLivePIDs = true;
  StartTransferMode = false;
#endif

  // Turn off live PIDs if necessary:

  if (TurnOffLivePIDs)
     TurnOffLiveMode(LiveView);

  // Set the tuner:

  dvbTuner->Set(Channel, DoTune);

  // If this channel switch was requested by the EITScanner we don't wait for
  // a lock and don't set any live PIDs (the EITScanner will wait for the lock
  // by itself before setting any filters):

  if (EITScanner.UsesDevice(this)) //XXX
     return true;

  // PID settings:

  if (TurnOnLivePIDs) {
     SetAudioBypass(false);
     if (!(AddPid(Channel->Ppid(), ptPcr) && AddPid(vpid, ptVideo) && AddPid(apid, ptAudio))) {
        esyslog("ERROR: failed to set PIDs for channel %d on device %d", Channel->Number(), CardIndex() + 1);
        return false;
        }
     if (IsPrimaryDevice())
        AddPid(Channel->Tpid(), ptTeletext);
     CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, true)); // actually one would expect 'false' here, but according to Marco Schl��ler <marco@lordzodiac.de> this works
                                                   // to avoid missing audio after replaying a DVD; with 'false' there is an audio disturbance when switching
                                                   // between two channels on the same transponder on DVB-S
     CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
     }
  else if (StartTransferMode)
     cControl::Launch(new cTransferControl(this, Channel->GetChannelID(), vpid, Channel->Apids(), Channel->Dpids(), Channel->Spids()));

  return true;
}

bool cDvbDevice::HasLock(int TimeoutMs)
{
  return dvbTuner ? dvbTuner->Locked(TimeoutMs) : false;
}

int cDvbDevice::GetAudioChannelDevice(void)
{
  if (HasDecoder()) {
     audio_status_t as;
     CHECK(ioctl(fd_audio, AUDIO_GET_STATUS, &as));
     return as.channel_select;
     }
  return 0;
}

void cDvbDevice::SetAudioChannelDevice(int AudioChannel)
{
  if (HasDecoder())
     CHECK(ioctl(fd_audio, AUDIO_CHANNEL_SELECT, AudioChannel));
}

void cDvbDevice::SetVolumeDevice(int Volume)
{
  if (HasDecoder()) {
     if (digitalAudio)
        Volume = 0;
     audio_mixer_t am;
     // conversion for linear volume response:
     am.volume_left = am.volume_right = 2 * Volume - Volume * Volume / 255;
     CHECK(ioctl(fd_audio, AUDIO_SET_MIXER, &am));
     }
}

void cDvbDevice::SetDigitalAudioDevice(bool On)
{
  if (digitalAudio != On) {
     if (digitalAudio)
        cCondWait::SleepMs(1000); // Wait until any leftover digital data has been flushed
     digitalAudio = On;
     SetVolumeDevice(On || IsMute() ? 0 : CurrentVolume());
     }
}

void cDvbDevice::SetTransferModeForDolbyDigital(int Mode)
{
  setTransferModeForDolbyDigital = Mode;
}

void cDvbDevice::SetAudioTrackDevice(eTrackType Type)
{
  const tTrackId *TrackId = GetTrack(Type);
  if (TrackId && TrackId->id) {
     SetAudioBypass(false);
     if (IS_AUDIO_TRACK(Type) || (IS_DOLBY_TRACK(Type) && SetAudioBypass(true))) {
        if (pidHandles[ptAudio].pid && pidHandles[ptAudio].pid != TrackId->id) {
           DetachAll(pidHandles[ptAudio].pid);
           if (CamSlot())
              CamSlot()->SetPid(pidHandles[ptAudio].pid, false);
           pidHandles[ptAudio].pid = TrackId->id;
           SetPid(&pidHandles[ptAudio], ptAudio, true);
           if (CamSlot()) {
              CamSlot()->SetPid(pidHandles[ptAudio].pid, true);
              CamSlot()->StartDecrypting();
              }
           }
        }
     else if (IS_DOLBY_TRACK(Type)) {
        if (setTransferModeForDolbyDigital == 0)
           return;
        // Currently this works only in Transfer Mode
        ForceTransferMode();
        }
     }
}

bool cDvbDevice::CanReplay(void) const
{
#ifndef DO_REC_AND_PLAY_ON_PRIMARY_DEVICE
  if (Receiving())
     return false;
#endif
  return cDevice::CanReplay();
}

bool cDvbDevice::SetPlayMode(ePlayMode PlayMode)
{
  if (PlayMode != pmExtern_THIS_SHOULD_BE_AVOIDED && fd_video < 0 && fd_audio < 0) {
     // reopen the devices
     fd_video = DvbOpen(DEV_DVB_VIDEO,  CardIndex(), O_RDWR | O_NONBLOCK);
     fd_audio = DvbOpen(DEV_DVB_AUDIO,  CardIndex(), O_RDWR | O_NONBLOCK);
     SetVideoFormat(Setup.VideoFormat);
     }

  switch (PlayMode) {
    case pmNone:
         // special handling to return from PCM replay:
         CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
         CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY));
         CHECK(ioctl(fd_video, VIDEO_PLAY));

         CHECK(ioctl(fd_video, VIDEO_STOP, true));
         CHECK(ioctl(fd_audio, AUDIO_STOP, true));
         CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
         CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
         CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX));
         CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX));
         CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
         CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, false));
         break;
    case pmAudioVideo:
    case pmAudioOnlyBlack:
         if (playMode == pmNone)
            TurnOffLiveMode(true);
         CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
         CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY));
         CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, PlayMode == pmAudioVideo));
         CHECK(ioctl(fd_audio, AUDIO_PLAY));
         CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY));
         CHECK(ioctl(fd_video, VIDEO_PLAY));
         break;
    case pmAudioOnly:
         CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
         CHECK(ioctl(fd_audio, AUDIO_STOP, true));
         CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
         CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY));
         CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, false));
         CHECK(ioctl(fd_audio, AUDIO_PLAY));
         CHECK(ioctl(fd_video, VIDEO_SET_BLANK, false));
         break;
    case pmVideoOnly:
         CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
         CHECK(ioctl(fd_video, VIDEO_STOP, true));
         CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX));
         CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, false));
         CHECK(ioctl(fd_audio, AUDIO_PLAY));
         CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
         CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY));
         CHECK(ioctl(fd_video, VIDEO_PLAY));
         break;
    case pmExtern_THIS_SHOULD_BE_AVOIDED:
         close(fd_video);
         close(fd_audio);
         fd_video = fd_audio = -1;
         break;
    }
  playMode = PlayMode;
  return true;
}

int64_t cDvbDevice::GetSTC(void)
{
  if (fd_stc >= 0) {
     struct dmx_stc stc;
     stc.num = 0;
     if (ioctl(fd_stc, DMX_GET_STC, &stc) == -1) {
        esyslog("ERROR: stc %d: %m", CardIndex() + 1);
        return -1;
        }
     return stc.stc / stc.base;
     }
  return -1;
}

void cDvbDevice::TrickSpeed(int Speed)
{
  if (fd_video >= 0)
     CHECK(ioctl(fd_video, VIDEO_SLOWMOTION, Speed));
}

void cDvbDevice::Clear(void)
{
  if (fd_video >= 0)
     CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
  if (fd_audio >= 0)
     CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
  cDevice::Clear();
}

void cDvbDevice::Play(void)
{
  if (playMode == pmAudioOnly || playMode == pmAudioOnlyBlack) {
     if (fd_audio >= 0)
        CHECK(ioctl(fd_audio, AUDIO_CONTINUE));
     }
  else {
     if (fd_audio >= 0) {
        CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
        CHECK(ioctl(fd_audio, AUDIO_CONTINUE));
        }
     if (fd_video >= 0)
        CHECK(ioctl(fd_video, VIDEO_CONTINUE));
     }
  cDevice::Play();
}

void cDvbDevice::Freeze(void)
{
  if (playMode == pmAudioOnly || playMode == pmAudioOnlyBlack) {
     if (fd_audio >= 0)
        CHECK(ioctl(fd_audio, AUDIO_PAUSE));
     }
  else {
     if (fd_audio >= 0) {
        CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, false));
        CHECK(ioctl(fd_audio, AUDIO_PAUSE));
        }
     if (fd_video >= 0)
        CHECK(ioctl(fd_video, VIDEO_FREEZE));
     }
  cDevice::Freeze();
}

void cDvbDevice::Mute(void)
{
  if (fd_audio >= 0) {
     CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, false));
     CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, true));
     }
  cDevice::Mute();
}

void cDvbDevice::StillPicture(const uchar *Data, int Length)
{
  if (!Data || Length < TS_SIZE)
     return;
  if (Data[0] == 0x47) {
     // TS data
     cDevice::StillPicture(Data, Length);
     }
  else if (Data[0] == 0x00 && Data[1] == 0x00 && Data[2] == 0x01 && (Data[3] & 0xF0) == 0xE0) {
     // PES data
     char *buf = MALLOC(char, Length);
     if (!buf)
        return;
     int i = 0;
     int blen = 0;
     while (i < Length - 6) {
           if (Data[i] == 0x00 && Data[i + 1] == 0x00 && Data[i + 2] == 0x01) {
              int len = Data[i + 4] * 256 + Data[i + 5];
              if ((Data[i + 3] & 0xF0) == 0xE0) { // video packet
                 // skip PES header
                 int offs = i + 6;
                 // skip header extension
                 if ((Data[i + 6] & 0xC0) == 0x80) {
                    // MPEG-2 PES header
                    if (Data[i + 8] >= Length)
                       break;
                    offs += 3;
                    offs += Data[i + 8];
                    len -= 3;
                    len -= Data[i + 8];
                    if (len < 0 || offs + len > Length)
                       break;
                    }
                 else {
                    // MPEG-1 PES header
                    while (offs < Length && len > 0 && Data[offs] == 0xFF) {
                          offs++;
                          len--;
                          }
                    if (offs <= Length - 2 && len >= 2 && (Data[offs] & 0xC0) == 0x40) {
                       offs += 2;
                       len -= 2;
                       }
                    if (offs <= Length - 5 && len >= 5 && (Data[offs] & 0xF0) == 0x20) {
                       offs += 5;
                       len -= 5;
                       }
                    else if (offs <= Length - 10 && len >= 10 && (Data[offs] & 0xF0) == 0x30) {
                       offs += 10;
                       len -= 10;
                       }
                    else if (offs < Length && len > 0) {
                       offs++;
                       len--;
                       }
                    }
                 if (blen + len > Length) // invalid PES length field
                    break;
                 memcpy(&buf[blen], &Data[offs], len);
                 i = offs + len;
                 blen += len;
                 }
              else if (Data[i + 3] >= 0xBD && Data[i + 3] <= 0xDF) // other PES packets
                 i += len + 6;
              else
                 i++;
              }
           else
              i++;
           }
     video_still_picture sp = { buf, blen };
     CHECK(ioctl(fd_video, VIDEO_STILLPICTURE, &sp));
     free(buf);
     }
  else {
     // non-PES data
     video_still_picture sp = { (char *)Data, Length };
     CHECK(ioctl(fd_video, VIDEO_STILLPICTURE, &sp));
     }
}

bool cDvbDevice::Poll(cPoller &Poller, int TimeoutMs)
{
  Poller.Add((playMode == pmAudioOnly || playMode == pmAudioOnlyBlack) ? fd_audio : fd_video, true);
  return Poller.Poll(TimeoutMs);
}

bool cDvbDevice::Flush(int TimeoutMs)
{
  //TODO actually this function should wait until all buffered data has been processed by the card, but how?
  return true;
}

int cDvbDevice::PlayVideo(const uchar *Data, int Length)
{
  return WriteAllOrNothing(fd_video, Data, Length, 1000, 10);
}

int cDvbDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  return WriteAllOrNothing(fd_audio, Data, Length, 1000, 10);
}

int cDvbDevice::PlayTsVideo(const uchar *Data, int Length)
{
  return WriteAllOrNothing(fd_video, Data, Length, 1000, 10);
}

int cDvbDevice::PlayTsAudio(const uchar *Data, int Length)
{
  return WriteAllOrNothing(fd_audio, Data, Length, 1000, 10);
}

bool cDvbDevice::OpenDvr(void)
{
  CloseDvr();
  fd_dvr = DvbOpen(DEV_DVB_DVR, CardIndex(), O_RDONLY | O_NONBLOCK, true);
  if (fd_dvr >= 0)
     tsBuffer = new cTSBuffer(fd_dvr, MEGABYTE(2), CardIndex() + 1);
  return fd_dvr >= 0;
}

void cDvbDevice::CloseDvr(void)
{
  if (fd_dvr >= 0) {
     delete tsBuffer;
     tsBuffer = NULL;
     close(fd_dvr);
     fd_dvr = -1;
     }
}

bool cDvbDevice::GetTSPacket(uchar *&Data)
{
  if (tsBuffer) {
     Data = tsBuffer->Get();
     return true;
     }
  return false;
}
