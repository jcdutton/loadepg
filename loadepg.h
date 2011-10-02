//#include <vdr/plugin.h>
//#include <vdr/device.h>
//#include <vdr/remote.h>
//#include <vdr/thread.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
//#include "../../../libsi/section.h"

#include "formats.h"
#include "i18n.h"

//#ifndef APIVERSNUM
//#include <vdr/config.h>
//#endif

#define DEBUG true
#define DEBUG_STARTTIME false

#define DVB_DEVICE_DEMUX "/dev/dvb/adapter%i/demux0"
#define LOADEPG_FILE_CONF "loadepg.conf"
#define LOADEPG_FILE_EQUIV "loadepg.equiv"
#define FILE_EPG_TMP "/tmp/epg.tmp"
#define FILE_EPG_ERR "/tmp/epg.err"
#define FILE_EPG_CHANNELS "/tmp/epg.channels"

#define MAX_FILTERS 24
#define MAX_ACTIVE_FILTERS 6
#define MAX_PROVIDERS 64

#define MAX_BOUQUETS 256
#define MAX_THEMES 4096
#define MAX_CHANNELS 4096
#define MAX_TITLES 262144
#define MAX_SUMMARIES 262144

#define MAX_BUFFER_SIZE_CHANNELS 1048576
#define MAX_BUFFER_SIZE_TITLES 4194304
#define MAX_BUFFER_SIZE_SUMMARIES 33554432

#define DATA_FORMAT_SKYBOX 1
#define DATA_FORMAT_MHW_1 2 
#define DATA_FORMAT_MHW_2 3
#define DATA_FORMAT_FILE 4
#define DATA_FORMAT_SCRIPT 5

#define TIMEOUT_CONTROL 90 // seconds
#define TIMEOUT_FILTER 5000 // ms
#define TIMEOUT_ROTOR 30 // seconds

struct sNode
{
  char *Value;
  struct sNode *P0;
  struct sNode *P1;
};

typedef struct sNode sNodeH;

typedef struct
{
  char *Directory;
  int DvbAdapterHasRotor;
  int DvbAdapterNumber;
  int EnableOsdMessages;
  int UseFileEquivalents;
} sConfig;

typedef struct
{
  unsigned short int OriginalSourceId;
  unsigned short int OriginalNid;
  unsigned short int OriginalTid;
  unsigned short int OriginalSid;
  unsigned short int OriginalRid;
  unsigned short int EquivSourceId;
  unsigned short int EquivNid;
  unsigned short int EquivTid;
  unsigned short int EquivSid;
  unsigned short int EquivRid;
} sEquivChannel;

typedef struct
{
  unsigned char Name[256];
} sTheme;

typedef struct
{
  unsigned short int ChannelId;
  unsigned short int Nid;
  unsigned short int Tid;
  unsigned short int Sid;
  unsigned short int SkyNumber;
  unsigned int pData;
  unsigned int lenData;
  bool IsFound;
  bool IsEpg;
} sChannel;

typedef struct
{
  unsigned short int BouquetId;
  short int SectionNumber[256];
  short int LastSectionNumber;
} sBouquet;

typedef struct
{
  unsigned short int ChannelId;
  unsigned short int ThemeId;
  unsigned short int MjdTime;
  unsigned short int EventId;
  unsigned int StartTime;
  unsigned int Duration;
  unsigned char SummaryAvailable;
  unsigned int pData;
  unsigned int lenData;
} sTitle;

typedef struct
{
  unsigned short int ChannelId;
  unsigned short int MjdTime;
  unsigned short int EventId;
  unsigned int pData;
  unsigned lenData;
} sSummary;

typedef struct
{
  char *Title;
  int DataFormat;
  int SourceId;
  char *Parm1;
  char *Parm2;
  char *Parm3;
} sProvider;

typedef struct
{
  int Fd;
  int Step;
  unsigned short int Pid;
  unsigned char Tid;
  unsigned char Mask;
} sFilter;

typedef struct
{
  int Fd;
  int FilterId;
  bool IsBusy;
} sActiveFilter;

class cControlLoadepg :
{
  private:
  protected:
    virtual void Action( void );
  public:
    cControlLoadepg( void );
    ~cControlLoadepg();
    bool Loading;
    bool Stopping;
    bool Error;
};

class cTaskLoadepg :
{
  private:
    int SetupUpdateChannels;
    int DvbAdapterNumber;
    cChannel *VdrChannel;
    cChannel *EpgChannel;
    cDevice *EpgDevice;
    sNodeH H;
    sNodeH *nH;
    bool HasSwitched;
    bool IsError;
    bool IsRunning;
    bool EndBAT;
    bool EndThemes;
    bool EndChannels;
    int pC;
    int pT;
    int pS;
    struct pollfd PFD[MAX_FILTERS];
    int nFilters;
    sFilter Filters[MAX_FILTERS];
    int nActiveFilters;
    sActiveFilter ActiveFilters[MAX_ACTIVE_FILTERS];
    unsigned char InitialBuffer[MAX_FILTERS][64];
    void AddFilter( unsigned short int Pid, unsigned char Tid = 0x00, unsigned char Mask = 0xff );
    void StartFilter( int FilterId );
    void StopFilter( int ActiveFilterId );
    void PollingFilters( int Timeout );
    void ReadBuffer( int FilterId, int Fd );
    void GetLocalTimeOffset( void );
    void GetSatelliteTimeOffset( int FilterId, unsigned char *Data, int Length );
    void GetChannelsSKYBOX( int FilterId, unsigned char *Data, int Length );
    void GetTitlesSKYBOX( int FilterId, unsigned char *Data, int Length );
    void GetSummariesSKYBOX( int FilterId, unsigned char *Data, int Length );
    void GetThemesMHW1( int FilterId, unsigned char *Data, int Length );
    void GetChannelsMHW1( int FilterId, unsigned char *Data, int Length );
    void GetTitlesMHW1( int FilterId, unsigned char *Data, int Length );
    void GetSummariesMHW1( int FilterId, unsigned char *Data, int Length );
    void GetThemesMHW2( int FilterId, unsigned char *Data, int Length );
    void GetChannelsMHW2( int FilterId, unsigned char *Data, int Length );
    void GetTitlesMHW2( int FilterId, unsigned char *Data, int Length );
    void GetSummariesMHW2( int FilterId, unsigned char *Data, int Length );
    void CreateEpgDataFile( void );
    bool ReadFileDictionary( void );
    bool ReadFileThemes( void );
    void CleanString( unsigned char *String );
    unsigned char DecodeText[4096];
    unsigned char DecodeErrorText[4096];
    int DecodeHuffmanCode( unsigned char *Data, int Length );
    void CreateFileChannels( const char *FileChannels );
  protected:
    virtual void Action( void );
  public:
    cTaskLoadepg( void );
    ~cTaskLoadepg();
    void LoadFromSatellite( void );
    void LoadFromFile( const char *FileEpg );
    void LoadFromScript( const char *FileScript, const char *FileEpg );
};

class cMainMenuLoadepg : public cOsdMenu
{
  private:
  protected:
  public:
    cMainMenuLoadepg( void );
    ~cMainMenuLoadepg();
    eOSState ProcessKey( eKeys Key );
};

class cSetupMenuLoadepg : public cMenuSetupPage
{
  private:
    const char *dvbAdapterNumberFormatTexts[8];
    const char *dvbAdapterHasRotorFormatTexts[8];
  protected:
    virtual void Store( void );
  public:
    cSetupMenuLoadepg( void );
    ~cSetupMenuLoadepg();
};
