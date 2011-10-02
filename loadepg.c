/* loadepg v0.6 -- tool to load the sky epg from a file captured from DCS1 channel.
 *
 * Copyright (C) 2009-2010  James Courtier-Dutton <James@superbug.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

#if 0
#define TS_LOG 1
#define TS_SCRAM 1
#define TS_PAT_LOG 1
#define TS_PMT_LOG 1
#define TS_HEADER_LOG 1
#define TS_PAT_LOG 1
#define TS_PMT_LOG 1
#define TS_SCRAM 1
#define TS_SI 1
#endif

/* more PIDS are needed due "auto-detection". 40 spare media entries  */
#define PKT_SIZE 188
#define BODY_SIZE (188 - 4)
#define MAX_PIDS ((BODY_SIZE - 1 - 13) / 4) + 40
#define MAX_PMTS ((BODY_SIZE - 1 - 13) / 4) + 10
#define SYNC_BYTE   0x47
/* more PIDS are needed due "auto-detection". 40 spare media entries  */
#define NULL_PID 0x1fff
#define INVALID_PID ((unsigned int)(-1))
#define INVALID_PROGRAM ((unsigned int)(-1))
#define INVALID_CC ((unsigned int)(-1))

#define PID_TYPE_NONE 0
#define PID_TYPE_PAT 1
#define PID_TYPE_PMT 2
#define PID_TYPE_CA_ECM 3
#define PID_TYPE_CA_EMM 4
#define PID_TYPE_NIT 5
#define PID_TYPE_AUDIO_MPEG 6
#define PID_TYPE_AUDIO_AC3 7
#define PID_TYPE_VIDEO_MPEG2 8
#define PID_TYPE_VIDEO_H264 9
#define PID_TYPE_PRIV_SECT 10
#define PID_TYPE_PRIV_PES 11
#define PID_TYPE_UNKNOWN 12
#define MAX_FILTERS 24
#define MAX_BOUQUETS 256
#define MAX_THEMES 4096
#define MAX_CHANNELS 4096
#define MAX_TITLES 262144
#define MAX_SUMMARIES 262144
#define MAX_SERVICES 4096

#define MAX_BUFFER_SIZE_CHANNELS 1048576
#define MAX_BUFFER_SIZE_TITLES 4194304
#define MAX_BUFFER_SIZE_SUMMARIES 33554432

char *type[] = {"NONE",
		"PAT",
		"PMT",
		"CA_ECM",
		"CA_EMM",
		"NIT",
		"AUDIO_MPEG",
		"AUDIO_AC3",
		"VIDEO_MPEG2",
		"VIDEO_H264",
		"PRIV_SECT",
		"PRIV_PES",
		"UNKNOWN"};

struct info_s {
	uint16_t value;
	char *desc;
};

struct info_s info_list[] = {
	0x1, "Video",
	0x2, "Music",
	0x4, "NVOD Reference",
	0x5, "NVOD time-shifted",
	0x19, "Video HD",
	0x82, "Unknown82",
	0x83, "EPG",
	0x85, "Unknown85",
	0x87, "HD Unknown87",
	0x0, NULL,
};
	
uint8_t buffer[200];

struct section_c0_s {
	int offset;
	int total_length;
	int summary_length;
	char *summary;
};

struct section_c0_s section_c0[0x100];

struct section_s {
	int size;
	uint8_t *whole_section;
	int buffer_target;
	uint8_t *buffer;
	int buffer_progress;
};

struct pid_s {
	int present;
	int scrambling_control;
	int program_count;
	int type;
	uint8_t last_continuity_counter;
	uint8_t *previous_ecm;
	struct section_s section;
	int pid_for_ecm;
};

struct video_pid_s {
	int pid;
	int ca_pid;
};

struct audio_pid_s {
	int pid;
	int ca_pid;
};

struct service_s {
	int program_id;
	char *provider;
	char *name;
};

struct program_s {
	int program_id;
	int pid_pmt;
	struct video_pid_s video;
	struct audio_pid_s audio;
	int service_count;
};

struct event_s {
	uint16_t event_id;
	uint16_t channel_id;
	uint64_t start_time_title; /* Unix 64bit start time in UTC, only accurate to the second and not microsecond */
	uint64_t start_time_summary; /* Unix 64bit start time in UTC, only accurate to the second and not microsecond */
	uint64_t duration_title; /* In seconds */
	uint16_t theme_id; /* Theme, FIXME: JCD: Details TODO */
	int prefix_len; /* How much of the Title should be ignored in searches. "The Italian Job", prefix_len = 4 */
	int title_len;
	char *title;
/* FIXME: JCD Add title, title suppliment and description is they all exist */
	int summary_len;
	char *summary;
};

struct channel_s {
	uint16_t ChannelId; /* This is a globally unique ID with Sky TV UK */
	uint16_t Nid;
	uint16_t Tid; /* Transport Mux ID: This will eventually link to tuning info */
	uint16_t Sid; /* Service ID: E.g. Provider name, Channel name. Use Provider to detect Sky TV or not */
	uint16_t SkyNumber1;
	uint16_t SkyNumber2;
	uint16_t info; /* Video, Music, HD etc. */
	uint16_t pData;
	uint16_t lenData;
	int IsFound;
	int IsEpg;
	int events_count;
	struct event_s *events;
};

struct bouquet_s {
  uint16_t BouquetId;
  int16_t SectionNumber[256];
  int16_t LastSectionNumber;
};

struct filter_s {
  int Fd;
  int Step;
  uint16_t Pid;
  uint8_t Tid;
  uint8_t Mask;
};

struct sNode
{
  char *Value;
  struct sNode *P0;
  struct sNode *P1;
};

/*
 * Describe a single elementary stream.
 */
struct demux_ts_s {
//  unsigned int     pid;
//  fifo_buffer_t   *fifo;
//  uint8_t         *content;
//  uint32_t         size;
//  uint32_t         type;
//  int64_t          pts;
//  buf_element_t   *buf;
//  unsigned int     counter;
//  uint16_t         descriptor_tag; /* +0x100 for PES stream IDs (no available TS descriptor tag?) */
//  int64_t          packet_count;
//  int              corrupted_pes;
//  uint32_t         buffered_bytes;
//  int              autodetected;
  uint32_t         crc32_table[256];
  uint32_t         program_number[MAX_PMTS];
  uint32_t         pmt_pid[MAX_PMTS];
	int	number_of_programs;
  struct program_s  *programs;
  struct service_s  *services;
	struct pid_s	*pids;
  int		  *pmt[MAX_PMTS];
  uint8_t         *pmt_write_ptr[MAX_PMTS];
  int              audio_tracks_count;
  unsigned int     videoPid;
  uint32_t         last_pmt_crc;
  unsigned int      spu_pid;

};
struct demux_ts_s demux_ts;

uint8_t pat[200];
struct sNode H;

int EndBAT;
int EndSDT;
int EndThemes;
int EndChannels;

int nChannels;
struct channel_s *lChannels;
uint16_t *channels_all;

int nBouquets;
struct bouquet_s *lBouquets;

int EpgTimeOffset;
int LocalTimeOffset;
int SatelliteTimeOffset;
int Yesterday;
int YesterdayEpoch;


int nFilters;
struct filter_s Filters[MAX_FILTERS];

static char *info_to_string(uint16_t value) {
	int n;
	n = 0;
	do {
		if (info_list[n].value == value) {
			break;
		}
		n++;
	} while (info_list[n].value != 0);
	if (info_list[n].value != 0) {
		return info_list[n].desc;
	} else {
		return "UNKNOWN";
	}
}


static int BcdToInt( unsigned char Bcd )
{
  return ( ( ( Bcd & 0xf0 ) >> 4 ) * 10 ) + ( Bcd & 0x0f );
}

static char *GetStringMJD( int MJD )
{
  int J, C, Y, M;
  int Day, Month, Year;
  char *Buffer;
  J = MJD + 2400001 + 68569;
  C = 4 * J / 146097;
  J = J - ( 146097 * C + 3 ) / 4;
  Y = 4000 * ( J + 1 ) / 1461001;
  J = J - 1461 * Y / 4 + 31;
  M = 80 * J / 2447;
  Day = J - 2447 * M / 80;
  J = M / 11;
  Month = M + 2 - ( 12 * J );
  Year = 100 * ( C - 49 ) + Y + J;
  asprintf( &Buffer, "%02i/%02i/%04i", Day, Month, Year );
  return Buffer;
}

static int bsearchChannelByChID( const void *A, const void *B )
{
  struct channel_s *ChannelA = ( struct channel_s * ) A;
  struct channel_s *ChannelB = ( struct channel_s * ) B;
#if 0
  if( ChannelA->Nid > ChannelB->Nid )
  {
    return 1;
  }
  if( ChannelA->Nid < ChannelB->Nid )
  {
    return -1;
  }
  if( ChannelA->Nid == ChannelB->Nid )
  {
    if( ChannelA->Tid > ChannelB->Tid )
    {
      return 1;
    }
    if( ChannelA->Tid < ChannelB->Tid )
    {
      return -1;
    }
    if( ChannelA->Tid == ChannelB->Tid )
    {
      if( ChannelA->Sid > ChannelB->Sid )
      {
        return 1;
      }
      if( ChannelA->Sid < ChannelB->Sid )
      {
        return -1;
      }
      if( ChannelA->Sid == ChannelB->Sid )
      {
#endif
	printf("CMP 0x%x, 0x%x\n", ChannelA->ChannelId, ChannelB->ChannelId );
        if( ChannelA->ChannelId > ChannelB->ChannelId )
	{
	  return 1;
	}
        if( ChannelA->ChannelId < ChannelB->ChannelId )
	{
	  return -1;
	}
//      }
//    }
//  }
  return 0;
}

static int qsortChannelsByChID( const void *A, const void *B )
{
  struct channel_s *ChannelA = ( struct channel_s * ) A;
  struct channel_s *ChannelB = ( struct channel_s * ) B;
  if( ChannelA->Nid > ChannelB->Nid )
  {
    return 1;
  }
  if( ChannelA->Nid < ChannelB->Nid )
  {
    return -1;
  }
  if( ChannelA->Nid == ChannelB->Nid )
  {
    if( ChannelA->Tid > ChannelB->Tid )
    {
      return 1;
    }
    if( ChannelA->Tid < ChannelB->Tid )
    {
      return -1;
    }
    if( ChannelA->Tid == ChannelB->Tid )
    {
      if( ChannelA->Sid > ChannelB->Sid )
      {
        return 1;
      }
      if( ChannelA->Sid < ChannelB->Sid )
      {
        return -1;
      }
      if( ChannelA->Sid == ChannelB->Sid )
      {
        if( ChannelA->ChannelId > ChannelB->ChannelId )
	{
	  return 1;
	}
        if( ChannelA->ChannelId < ChannelB->ChannelId )
	{
	  return -1;
	}
      }
    }
  }
  return 0;
}

static int qsort_events_by_time( const void *A, const void *B ) {
	struct event_s *eventA = ( struct event_s * ) A;
	struct event_s *eventB = ( struct event_s * ) B;
	if( eventA->start_time_title > eventB->start_time_title ) {
		return 1;
	}
	if( eventA->start_time_title < eventB->start_time_title ) {
		return -1;
	}
	return 0;
}

static void demux_ts_build_crc32_table(struct demux_ts_s *this) {
  uint32_t  i, j, k;

  for( i = 0 ; i < 256 ; i++ ) {
    k = 0;
    for (j = (i << 24) | 0x800000 ; j != 0x80000000 ; j <<= 1) {
      k = (k << 1) ^ (((k ^ j) & 0x80000000) ? 0x04c11db7 : 0);
    }
    this->crc32_table[i] = k;
  }
}

static uint32_t demux_ts_compute_crc32(struct demux_ts_s *this, uint8_t *data,
				       int32_t length, uint32_t crc32) {
  int32_t i;

  for(i = 0; i < length; i++) {
    crc32 = (crc32 << 8) ^ this->crc32_table[(crc32 >> 24) ^ data[i]];
  }
  return crc32;
}
static int ecm_compare(uint8_t *previous, uint8_t *buffer)
{
	uint8_t *original_pkt = buffer;
	uint8_t *pkt;
	uint8_t	adaptation_field_control;
	int ret;
	int data_offset;
	int ecm_offset;
	int tmp;
	uint32_t       table_id;
	uint32_t       section_syntax_indicator;
	int32_t        section_length;
	int 	n;
	int	match;
	
	ret = 0;
	data_offset = 4;

	adaptation_field_control       = (original_pkt[3] >> 4) & 0x03;
	if( adaptation_field_control & 0x2 ){
		uint32_t adaptation_field_length = original_pkt[4];
		/*
		 * Skip adaptation header.
		 */
		data_offset += adaptation_field_length + 1;
	}
	pkt = original_pkt + data_offset - 4;
	ecm_offset = data_offset - 4;
	/*
	 * sections start with a pointer. Skip it!
	 */
	tmp = pkt[4];
	ecm_offset += tmp;
	pkt += tmp;;
	table_id = (unsigned int)pkt[5] ;
	section_syntax_indicator = (((unsigned int)pkt[6] >> 7) & 1) ;
	section_length = (((unsigned int)pkt[6] & 0x03) << 8) | pkt[7];
	for(n = 0; n < section_length + 3; n++) {
		printf("%02x ", previous[ecm_offset + n + 5 ]);
		if ((n % 32) == 31) {
		printf("\n");
		}
	}
	printf("\n");
	for(n = 0; n < section_length + 3; n++) {
		printf("%02x ", buffer[ecm_offset + n + 5 ]);
		if ((n % 32) == 31) {
		printf("\n");
		}
	}
	printf("\n");
	match = 0;
	for(n = 0; n < section_length + 3; n++) {
		if (buffer[ecm_offset + n + 5] != previous[ecm_offset + n + 5 ]) {
			ret = 1;
			break;
		}
	}

	return ret;
}
static void demux_ts_parse_ecm (struct demux_ts_s *this, uint8_t *original_pkt,
                                uint32_t offset, unsigned int pusi) {
  uint32_t       table_id;
  uint32_t       section_syntax_indicator;
  int32_t        section_length;
  uint32_t       transport_stream_id;
  uint32_t       version_number;
  uint32_t       current_next_indicator;
  uint32_t       section_number;
  uint32_t       last_section_number;
	uint32_t	desc_tag;
	uint32_t	desc_len;
	uint32_t	ca_system_id;
	uint32_t	ca_pid;
  uint32_t       crc32;
  uint32_t       calc_crc32;
	uint32_t	pid;

  uint8_t	*program_offset;
  unsigned char *program;
  unsigned int   program_number;
  unsigned int   pmt_pid;
  unsigned int   program_count;
	int 	count;
	int	n;
	uint8_t *pkt;

  /*
   * A PAT in a single section should start with a payload unit start
   * indicator set.
   */
  printf ("demux_ts: parsing ECM\n");
  if (!pusi) {
    printf ("demux_ts: demux error! ECM without payload unit start indicator\n");
    return;
  }
	pkt = original_pkt + offset;
  /*
   * sections start with a pointer. Skip it!
   */
  pkt += pkt[4];
  if (pkt - original_pkt > PKT_SIZE) {
    printf ("demux_ts: demux error! CAT with invalid pointer\n");
    return;
  }
  table_id = (unsigned int)pkt[5] ;
  section_syntax_indicator = (((unsigned int)pkt[6] >> 7) & 1) ;
  section_length = (((unsigned int)pkt[6] & 0x03) << 8) | pkt[7];

#ifdef TS_PAT_LOG
  printf ("demux_ts: ECM table_id: %.2x\n", table_id);
  printf ("              section_syntax: %d\n", section_syntax_indicator);
  printf ("              section_length: %d (%#.3x)\n",
          section_length, section_length);
#endif
  /* Check CRC. */
  calc_crc32 = demux_ts_compute_crc32 (this, pkt+5, section_length+3-4,
                                       0xffffffff);
#ifdef TS_PAT_LOG
	printf ("demux_ts: ECM CRC32: %.8x\n", calc_crc32);
#endif
	pid                            = ((original_pkt[1] << 8) |
				    original_pkt[2]) & 0x1fff;
	printf("demux_ts:ts_header:pid:0x%.4x\n", pid);
	for(n = 0; n < section_length + 3 + 8; n++) {
		printf("%02x ", pkt[n + 5 ]);
		if ((n % 32) == 31) {
		printf("\n");
		}
	}
	printf("\n");

}

/*
 * demux_ts_parse_cat
 *
 * Parse a conditional access table (CAT).
 * The CAT is expected to be exactly one section long,
 * and that section is expected to be contained in a single TS packet.
 *
 * The CAT is assumed to contain a single program definition, though
 * we can cope with the stupidity of SPTSs which contain NITs.
 */
static void demux_ts_parse_cat (struct demux_ts_s *this, uint8_t *original_pkt,
                                uint32_t offset, unsigned int pusi) {
  uint32_t       table_id;
  uint32_t       section_syntax_indicator;
  int32_t        section_length;
  uint32_t       transport_stream_id;
  uint32_t       version_number;
  uint32_t       current_next_indicator;
  uint32_t       section_number;
  uint32_t       last_section_number;
	uint32_t	desc_tag;
	uint32_t	desc_len;
	uint32_t	ca_system_id;
	uint32_t	ca_pid;
  uint32_t       crc32;
  uint32_t       calc_crc32;

  uint8_t	*program_offset;
  unsigned char *program;
  unsigned int   program_number;
  unsigned int   pmt_pid;
  unsigned int   program_count;
	int 	count;
	int	n;
	uint8_t *pkt;

  /*
   * A PAT in a single section should start with a payload unit start
   * indicator set.
   */
  printf ("demux_ts: parsing CAT\n");
  if (!pusi) {
    printf ("demux_ts: demux error! CAT without payload unit start indicator\n");
    return;
  }
	pkt = original_pkt + offset;
  /*
   * sections start with a pointer. Skip it!
   */
  pkt += pkt[4];
  if (pkt - original_pkt > PKT_SIZE) {
    printf ("demux_ts: demux error! CAT with invalid pointer\n");
    return;
  }
  table_id = (unsigned int)pkt[5] ;
  section_syntax_indicator = (((unsigned int)pkt[6] >> 7) & 1) ;
  section_length = (((unsigned int)pkt[6] & 0x03) << 8) | pkt[7];
  transport_stream_id = ((uint32_t)pkt[8] << 8) | pkt[9];
  version_number = ((uint32_t)pkt[10] >> 1) & 0x1f;
  current_next_indicator = ((uint32_t)pkt[10] & 0x01);
  section_number = (uint32_t)pkt[11];
  last_section_number = (uint32_t)pkt[12];
  crc32 = (uint32_t)pkt[4+section_length] << 24;
  crc32 |= (uint32_t)pkt[5+section_length] << 16;
  crc32 |= (uint32_t)pkt[6+section_length] << 8;
  crc32 |= (uint32_t)pkt[7+section_length] ;

#ifdef TS_PAT_LOG
  printf ("demux_ts: CAT table_id: %.2x\n", table_id);
  printf ("              section_syntax: %d\n", section_syntax_indicator);
  printf ("              section_length: %d (%#.3x)\n",
          section_length, section_length);
  printf ("              transport_stream_id: %#.4x\n", transport_stream_id);
  printf ("              version_number: %d\n", version_number);
  printf ("              c/n indicator: %d\n", current_next_indicator);
  printf ("              section_number: %d\n", section_number);
  printf ("              last_section_number: %d\n", last_section_number);
#endif
  if ((section_syntax_indicator != 1) || !(current_next_indicator)) {
    return;
  }

  if (pkt - original_pkt > BODY_SIZE - 1 - 3 - section_length) {
    printf ("demux_ts: FIXME: (unsupported )PAT spans multiple TS packets\n");
    return;
  }

  if ((section_number != 0) || (last_section_number != 0)) {
    printf ("demux_ts: FIXME: (unsupported) PAT consists of multiple (%d) sections\n", last_section_number);
    return;
  }

  /* Check CRC. */
  calc_crc32 = demux_ts_compute_crc32 (this, pkt+5, section_length+3-4,
                                       0xffffffff);
  if (crc32 != calc_crc32) {
    printf ("demux_ts: demux error! PAT with invalid CRC32: packet_crc32: %.8x calc_crc32: %.8x\n",
	     crc32,calc_crc32);
    return;
  }
#ifdef TS_PAT_LOG
  else {
    printf ("demux_ts: CAT CRC32: %.8x ok.\n", crc32);
  }
#endif
		for (n = 0; n < section_length - 9 ; ) {
			desc_tag = pkt[13 + n];
			desc_len = pkt[13 + n + 1];
			ca_system_id = 0;
			ca_pid = 0;
			/* Handle CA */
			if (desc_tag == 9) {
				ca_system_id = (((uint32_t) pkt[13 + n + 2] << 8) |
						 pkt[13 + n + 3]);
				ca_pid = (((uint32_t) pkt[13 + n + 4] << 8) |
						 pkt[13 + n + 5]) & 0x1fff;
				this->pids[ca_pid].type = PID_TYPE_CA_EMM;
			}
			printf ("              desc_tag: 0x%02x\n", desc_tag);
			printf ("              desc_len: 0x%02x\n", desc_len);
			if (desc_tag == 9) {
				printf ("              ca_system_id: 0x%04x\n", ca_system_id);
				printf ("              ca_pid: 0x%04x\n", ca_pid);
			}
			printf("n = %d\n", n);
			n += desc_len + 2;
			printf("n + desc_len + 2 = %d\n", n);
		}
		printf("\n");

}

/*
 * demux_ts_parse_pat
 *
 * Parse a program association table (PAT).
 * The PAT is expected to be exactly one section long,
 * and that section is expected to be contained in a single TS packet.
 *
 * The PAT is assumed to contain a single program definition, though
 * we can cope with the stupidity of SPTSs which contain NITs.
 */
static void demux_ts_parse_pat (struct demux_ts_s *this, uint8_t *original_pkt,
                                uint32_t offset, unsigned int pusi) {
  uint32_t       table_id;
  uint32_t       section_syntax_indicator;
  int32_t        section_length;
  uint32_t       transport_stream_id;
  uint32_t       version_number;
  uint32_t       current_next_indicator;
  uint32_t       section_number;
  uint32_t       last_section_number;
  uint32_t       crc32;
  uint32_t       calc_crc32;

  uint8_t	*program_offset;
  unsigned char *program;
  unsigned int   program_number;
  unsigned int   pmt_pid;
  unsigned int   program_count;
	int 	count;
	int	n;
	uint8_t *pkt;

  /*
   * A PAT in a single section should start with a payload unit start
   * indicator set.
   */
  printf ("demux_ts: parsing PAT\n");
  if (!pusi) {
    printf ("demux_ts: demux error! PAT without payload unit start indicator\n");
    return;
  }
	pkt = original_pkt + offset;
  /*
   * sections start with a pointer. Skip it!
   */
  pkt += pkt[4];
  if (pkt - original_pkt > PKT_SIZE) {
    printf ("demux_ts: demux error! PAT with invalid pointer\n");
    return;
  }
  table_id = (unsigned int)pkt[5] ;
  section_syntax_indicator = (((unsigned int)pkt[6] >> 7) & 1) ;
  section_length = (((unsigned int)pkt[6] & 0x03) << 8) | pkt[7];
  transport_stream_id = ((uint32_t)pkt[8] << 8) | pkt[9];
  version_number = ((uint32_t)pkt[10] >> 1) & 0x1f;
  current_next_indicator = ((uint32_t)pkt[10] & 0x01);
  section_number = (uint32_t)pkt[11];
  last_section_number = (uint32_t)pkt[12];
  crc32 = (uint32_t)pkt[4+section_length] << 24;
  crc32 |= (uint32_t)pkt[5+section_length] << 16;
  crc32 |= (uint32_t)pkt[6+section_length] << 8;
  crc32 |= (uint32_t)pkt[7+section_length] ;

#ifdef TS_PAT_LOG
  printf ("demux_ts: PAT table_id: %.2x\n", table_id);
  printf ("              section_syntax: %d\n", section_syntax_indicator);
  printf ("              section_length: %d (%#.3x)\n",
          section_length, section_length);
  printf ("              transport_stream_id: %#.4x\n", transport_stream_id);
  printf ("              version_number: %d\n", version_number);
  printf ("              c/n indicator: %d\n", current_next_indicator);
  printf ("              section_number: %d\n", section_number);
  printf ("              last_section_number: %d\n", last_section_number);
#endif

  if ((section_syntax_indicator != 1) || !(current_next_indicator)) {
    return;
  }

  if (pkt - original_pkt > BODY_SIZE - 1 - 3 - section_length) {
    printf ("demux_ts: FIXME: (unsupported )PAT spans multiple TS packets\n");
    return;
  }

  if ((section_number != 0) || (last_section_number != 0)) {
    printf ("demux_ts: FIXME: (unsupported) PAT consists of multiple (%d) sections\n", last_section_number);
    return;
  }

  /* Check CRC. */
  calc_crc32 = demux_ts_compute_crc32 (this, pkt+5, section_length+3-4,
                                       0xffffffff);
  if (crc32 != calc_crc32) {
    printf ("demux_ts: demux error! PAT with invalid CRC32: packet_crc32: %.8x calc_crc32: %.8x\n",
	     crc32,calc_crc32);
    return;
  }
#ifdef TS_PAT_LOG
  else {
    printf ("demux_ts: PAT CRC32 ok.\n");
  }
#endif

  /*
   * Process all programs in the program loop.
   */
	printf("section_length - 9 = %d\n", section_length - 9);
	program_offset = pkt + 13;
	count = (section_length - 9) / 4;
  program_count = 0;
  for (n = 0; n < (section_length - 9); n += 4) {
    program_number = ((unsigned int)program_offset[n] << 8) | program_offset[n + 1];
    pmt_pid = (((unsigned int)program_offset[n + 2] & 0x1f) << 8) | program_offset[n + 3];

    /*
     * completely skip NIT pids.
     */
//    if (program_number == 0x0000)
//      continue;

    /*
     * If we have yet to learn our program number, then learn it,
     * use this loop to eventually add support for dynamically changing
     * PATs.
     */
	program_count = 0;

	while ((this->programs[program_count].program_id != INVALID_PROGRAM) &&
		(this->programs[program_count].program_id != program_number)  &&
		(program_count < 255 ) ) {
		program_count++;
	}
	this->programs[program_count].program_id = program_number;
	this->programs[program_count].pid_pmt = pmt_pid;
	this->pids[pmt_pid].program_count = program_count;
	if (program_number) {
		this->pids[pmt_pid].type = PID_TYPE_PMT;
	} else {
		this->pids[pmt_pid].type = PID_TYPE_NIT;
	}


#ifdef TS_PAT_LOG
    if (this->programs[program_count].program_id != INVALID_PROGRAM)
      printf ("demux_ts: PAT acquired count=%d programNumber=0x%04x "
              "pmtPid=0x%04x\n",
              program_count,
              this->programs[program_count].program_id,
              this->programs[program_count].pid_pmt);
#endif
  }
}

/*
 * NAME demux_ts_parse_pmt
 *
 * Parse a PMT. The PMT is expected to be exactly one section long,
 * and that section is expected to be contained in a single TS packet.
 *
 * In other words, the PMT is assumed to describe a reasonable number of
 * video, audio and other streams (with descriptors).
 * FIXME: Implement support for multi section PMT.
 */

static void demux_ts_parse_section_si (struct demux_ts_s *this,
	uint8_t *original_pkt,
	uint32_t	offset,
	unsigned int   pusi,
	struct section_s *section,
	int		discontinuity,
	unsigned int	pid)
{

	uint32_t       table_id;
	uint32_t       section_syntax_indicator;
	uint32_t       section_length = 0; /* to calm down gcc */
	uint32_t       program_number;
	uint32_t       version_number;
	uint32_t       current_next_indicator;
	uint32_t       section_number;
	uint32_t       last_section_number;
	uint32_t	reserved1;
	uint32_t	pcr_pid;
	uint32_t	reserved2;
	uint32_t       program_info_length;
	uint32_t       crc32;
	uint32_t       calc_crc32;
	uint32_t       coded_length;
	unsigned char *stream;
	unsigned int	 i;
	int		 count;
	char		*ptr = NULL;
	unsigned char  len;
	uint32_t	tmp32;
	uint32_t	offset_section_start;
	uint8_t		*pkt;
	uint32_t	progress;
	int		n;

	/*
	 * A new section should start with the payload unit start
	 * indicator set. We allocate some mem (max. allowed for a PM section)
	 * to copy the complete section into one chunk.
	 */
#ifdef TS_SI
	printf("section->size = 0x%x\n", section->size);
	printf("section->buffer_target = 0x%x\n", section->buffer_target);
	printf("section->buffer_progress = 0x%x\n", section->buffer_progress);
#endif
	/* When the payload of the Transport Stream packet contains PES packet data, the payload_unit_start_indicator has the
following significance: a '1' indicates that the payload of this Transport Stream packet will commence with the first byte
of a PES packet and a '0' indicates no PES packet shall start in this Transport Stream packet. If the
payload_unit_start_indicator is set to '1', then one and only one PES packet starts in this Transport Stream packet. This
also applies to private streams of stream_type 6 
	 */

	/* When the payload of the Transport Stream packet contains PSI data, the payload_unit_start_indicator has the following
significance: if the Transport Stream packet carries the first byte of a PSI section, the payload_unit_start_indicator value
shall be '1', indicating that the first byte of the payload of this Transport Stream packet carries the pointer_field. If the
Transport Stream packet does not carry the first byte of a PSI section, the payload_unit_start_indicator value shall be '0',
indicating that there is no pointer_field in the payload. This also applies to private streams of
stream_type 5
	 */
	/* A special case where the start of the section is in this packet, but there are less than 3 bytes of it.
	 * You therefore only have the table_id but not the section_length.
         * Need to store bytes and delay processing till next packet
	 */
	if (!section->whole_section) {
		section->whole_section = calloc(0x1100, 1);   /* Max section length is 0xfff + 3 + 188 */
		if (!section->whole_section) {
			printf("OUT OF MEMORY!!!!\n");
			return;
		}
	}
	if (!section->buffer) {
		section->buffer = calloc(0x1100, 1);   /* Max section length is 0xfff + 3 + 188 */
		if (!section->buffer) {
			printf("OUT OF MEMORY!!!!\n");
			return;
		}
	}

	if (pusi) {
		/* pointer to start of section. */
		/* Only exists if pusi is set. */
#ifdef TS_SI
		printf ("demux_ts: section pusi\n");
#endif
		len = 188 - offset;
#ifdef TS_SI
		printf("pusi: offset = 0x%04x len = 0x%04x\n", offset, len);
#endif
		tmp32 = original_pkt[offset + 4];
		offset_section_start = offset + tmp32 + 5;
		pkt = original_pkt + offset_section_start;
#ifdef TS_SI
		printf("pusi: offset_section_start = 0x%04x\n", offset_section_start);
#endif
		

		if (!section->whole_section) {
			printf("CORRUPTED whole section!!!!\n");
			return;
		}
		if (!section->buffer) {
			printf("CORRUPTED section buffer!!!!\n");
			return;
		}

#ifdef TS_SI
		printf("1 offset = 0x%x\n", offset);
		printf("1 offset_section_start = 0x%x\n", offset_section_start);
		printf("1 offset_section_start  -  offset - 3 = 0x%x\n", offset_section_start - offset - 3);
#endif
		memcpy (section->buffer + section->buffer_progress, original_pkt + offset + 5, offset_section_start - offset - 3);
		section->buffer_progress += offset_section_start - offset - 3;
#ifdef TS_SI
		printf("2 section->buffer_target = 0x%x\n", section->buffer_target);
		printf("2 section->buffer_progress = 0x%x\n", section->buffer_progress);
#endif
		if ((section->buffer_target) && (section->buffer_progress >= section->buffer_target)) {
			/* We have a complete section_si */
#ifdef TS_SI
			printf("complete section si!\n");
#endif
			memset(section->whole_section, 0, 0x1100);
			memcpy(section->whole_section, section->buffer, section->buffer_target);
			section->size = section->buffer_target;
		}
		
		memset(section->buffer, 0, 0x1003);
		memcpy(section->buffer, pkt, 188 - offset_section_start);
		section->buffer_progress = 188 - offset_section_start;
		section->buffer_target = 0;
		if (section->buffer_progress > 11) {
			table_id                  =  section->buffer[0] ;
			section_syntax_indicator  = (section->buffer[1] >> 7) & 0x01;
			/* section_length is a 12-bit field, the first two bits of which shall be '00'.
			 * The remaining 10 bits specify the number of bytes of the section starting
			 * immediately following the section_length field, and including the CRC.
			 * The value in this field shall not exceed 1021 (0x3FD).
			 * BUT...this seems to be a 12-bit field for epg.
			 */
			section_length            = (((uint32_t) section->buffer[1] << 8) | section->buffer[2]) & 0x0fff; 
			section->buffer_target = section_length + 3;

			program_number            =  ((uint32_t) section->buffer[3] << 8) | section->buffer[4];
			version_number            = (section->buffer[5] >> 1) & 0x1f;
			current_next_indicator    =  section->buffer[5] & 0x01;
			section_number            =  section->buffer[6];
			last_section_number       =  section->buffer[7];
			pcr_pid                   = (((uint32_t) section->buffer[8] << 8) | section->buffer[9]) & 0x1fff;
			program_info_length       = (((uint32_t) section->buffer[10] << 8) | section->buffer[11]) & 0x0fff;

#ifdef TS_PMT_LOG
			printf ("demux_ts: SECTION table_id: %2x, pid = 0x%x\n", table_id, pid);
			printf ("              section_syntax: %d\n", section_syntax_indicator);
			printf ("              section_length: %d (%#.3x)\n",
				section_length, section_length);
			printf ("              program_number: %#.4x\n", program_number);
			printf ("              version_number: %d\n", version_number);
			printf ("              c/n indicator: %d\n", current_next_indicator);
			printf ("              section_number: %d\n", section_number);
			printf ("              last_section_number: %d\n", last_section_number);
			printf ("              pcr_pid: 0x%04x\n", pcr_pid);
			printf ("              program_info_length: 0x%04x\n", program_info_length);
			printf ("              buffer_target: 0x%04x\n", section->buffer_target);
#endif
		}

		if ((section_syntax_indicator != 1) || (!current_next_indicator)) {
//#ifdef TS_PMT_LOG
			printf ("ts_demux: section_syntax_indicator != 1 || !current_next_indicator\n");
//#endif
			//section->size = 0;
			//return;
		}
	} else {
#ifdef TS_SI
		printf ("demux_ts: section !pusi\n");
#endif
		if (discontinuity) {
			section->size = 0;
			printf ("demux_ts: section !pusi discontinuity\n");
			return;
		}
		/* Wait for pusi */
		if (!(section->buffer_progress)) {
			return;
		}
		len = 188 - offset - 4;
#ifdef TS_SI
		printf("!pusi: offset = 0x%04x len = 0x%04x\n", offset, len);
#endif
		memcpy (section->buffer + section->buffer_progress, original_pkt + offset + 4, len);
		section->buffer_progress += len;
		if (section->buffer_progress > 11) {
			table_id                  =  section->buffer[0] ;
			section_syntax_indicator  = (section->buffer[1] >> 7) & 0x01;
			/* section_length is a 12-bit field, the first two bits of which shall be '00'.
			 * The remaining 10 bits specify the number of bytes of the section starting
			 * immediately following the section_length field, and including the CRC.
			 * The value in this field shall not exceed 1021 (0x3FD).
			 * BUT...this seems to be a 12-bit field for epg.
			 */
			section_length            = (((uint32_t) section->buffer[1] << 8) | section->buffer[2]) & 0x0fff; 
			section->buffer_target = section_length + 3;

			program_number            =  ((uint32_t) section->buffer[3] << 8) | section->buffer[4];
			version_number            = (section->buffer[5] >> 1) & 0x1f;
			current_next_indicator    =  section->buffer[5] & 0x01;
			section_number            =  section->buffer[6];
			last_section_number       =  section->buffer[7];
			pcr_pid                   = (((uint32_t) section->buffer[8] << 8) | section->buffer[9]) & 0x1fff;
			program_info_length       = (((uint32_t) section->buffer[10] << 8) | section->buffer[11]) & 0x0fff;

#ifdef TS_PMT_LOG
			printf ("demux_ts: SECTION table_id: %2x, pid = 0x%x (small)\n", table_id, pid);
			printf ("              section_syntax: %d\n", section_syntax_indicator);
			printf ("              section_length: %d (%#.3x)\n",
				section_length, section_length);
			printf ("              program_number: %#.4x\n", program_number);
			printf ("              version_number: %d\n", version_number);
			printf ("              c/n indicator: %d\n", current_next_indicator);
			printf ("              section_number: %d\n", section_number);
			printf ("              last_section_number: %d\n", last_section_number);
			printf ("              pcr_pid: 0x%04x\n", pcr_pid);
			printf ("              program_info_length: 0x%04x\n", program_info_length);
			printf ("              buffer_target: 0x%04x\n", section->buffer_target);
#endif
		}
		if ((section->buffer_target) && (section->buffer_progress >= section->buffer_target)) {
			/* We have a complete section_si */
			printf("complete section si 2!\n");
			memset(section->whole_section, 0, 0x1100);
			memcpy(section->whole_section, section->buffer, section->buffer_target);
			section->size = section->buffer_target;
		}
	}
#ifdef TS_SI
	printf("section->size = 0x%x\n", section->size);
	printf("section->buffer_target = 0x%x\n", section->buffer_target);
	printf("section->buffer_progress = 0x%x\n", section->buffer_progress);
#endif
#if 0
	for(n = 0; n < section->buffer_progress; n++) {
		printf("%02x ", section->buffer[n]);
		if ((n % 32) == 31) {
			printf("\n");
		}
	}
	printf("\n");
#endif
}

static void process_sdt_descriptors(struct demux_ts_s *this, struct service_s *service, uint8_t *buffer, int len)
{
	int n, m;
	int desc_tag;
	int desc_len;
	int tmp;
	int len2;
	int len3;
	int type;

	for (n = 0; n < len; ) {
		desc_tag = buffer[n];
		desc_len = buffer[n + 1];
		printf("sdt: tag=0x%x, len=0x%x\n", desc_tag, desc_len);
		switch (desc_tag) {
		case 0x48:
			type =  buffer[n + 2];
			len2 = buffer[n + 3];
			printf("type:0x%x, len2:0x%x\n", type, len2);
			for (m = 0; m < len2; m++) {
				printf("%c", buffer[n + m + 4]);
			}
			printf("\n");
			if (!(service->provider)) {
				service->provider = malloc(len2+1);
			}
			memcpy(service->provider, &buffer[n + 4], len2);
			service->provider[len2] = 0;
			len3 = buffer[n + 4 + len2];
			printf("len3:0x%x\n", len3);
			for (m = 0; m < len3; m++) {
				printf("%c", buffer[n + m + 5 + len2]);
			}
			printf("\n");
			if (!(service->name)) {
				service->name = malloc(len3+1);
			}
			memcpy(service->name, &buffer[n + 5 + len2], len3);
			service->name[len3] = 0;
			break;
		default:
			printf("sdt: Unknown tag 0x%x\n", desc_tag);
			for (m = 0; m < desc_len; m++) {
				printf("%02x", buffer[n + m + 2]);
			}
			printf("\n");
			for (m = 0; m < desc_len; m++) {
				tmp =buffer[n + m + 2];
				if ((tmp > 31) && (tmp < 127)) {
					printf("%c ", tmp);
				} else {
					printf("  ", tmp);
				}
			}
			printf("\n");
			break;
		}

		n += desc_len + 2;
	}
}

static void parse_sdt_actual(struct demux_ts_s *this, int pid)
{
	int		program_count;
	int		service_count;
	struct program_s *program;
	uint8_t *buffer;
	int section_length;
	uint32_t table_id_ext;
	uint32_t section_version_number;
	uint32_t section_number;
	uint32_t last_section_number;
	uint32_t offset = 11;
	uint32_t service_id;
	int descriptors_loop_len;
	int n;


	program_count = this->pids[pid].program_count;
	program = &(this->programs[program_count]);
	buffer = this->pids[pid].section.buffer;
	section_length = this->pids[pid].section.size;

//	section_length = ((buffer[1] & 0x0f) << 8) | buffer[2];
	table_id_ext = (buffer[3] << 8) | buffer[4];
	section_version_number = (buffer[5] >> 1) & 0x1f;
	section_number = buffer[6];
	last_section_number = buffer[7];
	
	for (n = offset; n < section_length - 4; ) {
		service_id = (buffer[n] << 8) | buffer[n + 1];
		descriptors_loop_len = ((buffer[n + 3] & 0x0f) << 8) | buffer[n + 4];
		//printf("sdt actual: service_id:0x%x len:0x%x\n", service_id, descriptors_loop_len);
		service_count = 0;

		while ((this->services[service_count].program_id != INVALID_PROGRAM) &&
			(this->services[service_count].program_id != service_id)  &&
			(service_count < MAX_SERVICES ) ) {
			service_count++;
		}
		if (service_count == MAX_SERVICES) {
			printf("ERROR: MAX_SERVICES overflow\n");
			return;
		}
		this->services[service_count].program_id = service_id;
		process_sdt_descriptors( this, &(this->services[service_count]), &buffer[n + 5], descriptors_loop_len );
		//printf("sdt actual: service_count=0x%x, %s, %s\n", service_count, this->services[service_count].provider, this->services[service_count].name);
		//printf("sdt actual: service_count=0x%x, %s, %s\n", 0xe, this->services[0xe].provider, this->services[0xe].name);
		n += descriptors_loop_len + 5;
	}
}

#if 1
    unsigned char DecodeText[4096];
    unsigned char DecodeErrorText[4096];
	uint8_t buffer_for_decode[4096];

int decode_huffman_code( unsigned char *Data, int Length, uint8_t *decoded )
{
  int i;
  int p;
  int q;
  int CodeError;
  int IsFound;
  unsigned char Byte;
  unsigned char lastByte;
  unsigned char Mask;
  unsigned char lastMask;
	struct sNode *nH;
  nH = &H;
  p = 0;
  q = 0;
  DecodeText[0] = '\0';
  DecodeErrorText[0] = '\0';
	if (!Length) {
		return p;
	}
  CodeError = 0;
  IsFound = 0;
  lastByte = 0;
  lastMask = 0;
  for( i = 0; i < Length; i ++ )
  {
    Byte = Data[i];
    Mask = 0x80;
    if( i == 0 )
    {
      Mask = 0x20;
      lastByte = i;
      lastMask = Mask;
    }
    loop1:;
    if( IsFound )
    {
      lastByte = i;
      lastMask = Mask;
      IsFound = 0;
    }
    if( ( Byte & Mask ) == 0 )
    {
      //printf("0");
      if( CodeError )
      {
        DecodeErrorText[q] = 0x30;
	q ++;
	goto nextloop1;
      }
      if( nH->P0 != NULL )
      {
        nH = nH->P0;
	if( nH->Value != NULL )
	{
	  memcpy( &DecodeText[p], nH->Value, strlen( nH->Value ) );
	  //printf(" %s\n",nH->Value);
	  p += strlen( nH->Value );
	  nH = &H;
	  IsFound = 1;
	}
      }
      else
      {
	memcpy( &DecodeText[p], "<...?...>", 9 );
	p += 9;
	i = lastByte;
	Byte = Data[lastByte];
	Mask = lastMask;
	CodeError = 1;
        goto loop1;
      }
    }
    else
    {
      //printf("1");
      if( CodeError )
      {
        DecodeErrorText[q] = 0x31;
	q ++;
	goto nextloop1;
      }
      if( nH->P1 != NULL )
      {
        nH = nH->P1;
	if( nH->Value != NULL )
	{
	  memcpy( &DecodeText[p], nH->Value, strlen( nH->Value ) );
	  //printf(" %s\n",nH->Value);
	  p += strlen( nH->Value );
	  nH = &H;
	  IsFound = 1;
	}
      }
      else
      {
	memcpy( &DecodeText[p], "<...?...>", 9 );
	p += 9;
	i = lastByte;
	Byte = Data[lastByte];
	Mask = lastMask;
	CodeError = 1;
        goto loop1;
      }
    }
    nextloop1:;
    Mask = Mask >> 1;
    if( Mask > 0 )
    {
      goto loop1;
    }
  }
  DecodeText[p] = '\0';
  DecodeErrorText[q] = '\0';
//	printf("\nEND\n");
  return p;
}

#endif

#if 1
inline char *skipspace(const char *s)
{
  if ((uint8_t)*s > ' ') // most strings don't have any leading space, so handle this case as fast as possible
     return (char *)s;
  while (*s && (uint8_t)*s <= ' ') // avoiding isspace() here, because it is much slower
        s++;
  return (char *)s;
}
int isempty(const char *s)
{
  return !(s && *skipspace(s));
}

int read_huff_dict( void )
{
  char *FileName;
  FILE *FileDict;
  char *Line;
  char Buffer[256];
	struct sNode *nH;
  asprintf( &FileName, "%s/%s", "conf", "sky_uk.dict" );
  FileDict = fopen( FileName, "r" );
  if( FileDict == NULL )
  {
    printf( "LoadEPG: Error opening file '%s'. %s", FileName, strerror( errno ) );
    free( FileName );
    return 0;
  }
  else
  {
    int i;
    int LenPrefix;
    char string1[256];
    char string2[256];
    H.Value = NULL;
    H.P0 = NULL;
    H.P1 = NULL;
    while( ( Line = fgets( Buffer, sizeof( Buffer ), FileDict ) ) != NULL )
    {
      if( ! isempty( Line ) )
      {
        memset( string1, 0, sizeof( string1 ) );
	memset( string2, 0, sizeof( string2 ) );
	if( sscanf( Line, "%c=%[^\n]\n", string1, string2 ) == 2 )
	{
	  goto codingstart;
	}
        else if( sscanf( Line, "%[^=]=%[^\n]\n", string1, string2 ) == 2 )
	{
	  codingstart:;
	  nH = &H;
	  LenPrefix = strlen( string2 );
	  for( i = 0; i < LenPrefix; i ++ )
	  {
	    switch( string2[i] )
	    {
	      case '0':
	        if( nH->P0 == NULL )
		{
		  nH->P0 = malloc(sizeof(struct sNode));
		  nH = nH->P0;
		  nH->Value = NULL;
		  nH->P0 = NULL;
		  nH->P1 = NULL;
		  if( ( LenPrefix - 1 ) == i )
		  {
		    asprintf( &nH->Value, "%s", string1 );
		  }
		}
		else
		{
		  nH = nH->P0;
		  if( nH->Value != NULL || ( LenPrefix - 1 ) == i )
		  {
		    printf( "LoadEPG: Error, huffman prefix code already exists for \"%s\"=%s with '%s'", string1, string2, nH->Value );
		  }
		}
	        break;
	      case '1':
	        if( nH->P1 == NULL )
		{
		  nH->P1 = malloc(sizeof(struct sNode));
		  nH = nH->P1;
		  nH->Value = NULL;
		  nH->P0 = NULL;
		  nH->P1 = NULL;
		  if( ( LenPrefix - 1 ) == i )
		  {
		    asprintf( &nH->Value, "%s", string1 );
		  }
		}
		else
		{
		  nH = nH->P1;
		  if( nH->Value != NULL || ( LenPrefix - 1 ) == i )
		  {
		    printf( "LoadEPG: Error, huffman prefix code already exists for \"%s\"=%s with '%s'", string1, string2, nH->Value );
		  }
		}
	        break;
	      default:
	        break;
	    }
	  }
	}
      }
    }
    fclose( FileDict );
  }
  
  // check tree huffman nodes
  FileDict = fopen( FileName, "r" );
  if( FileDict )
  {
    int i;
    int LenPrefix;
    char string1[256];
    char string2[256];
    while( ( Line = fgets( Buffer, sizeof( Buffer ), FileDict ) ) != NULL )
    {
      if( ! isempty( Line ) )
      {
        memset( string1, 0, sizeof( string1 ) );
	memset( string2, 0, sizeof( string2 ) );
	if( sscanf( Line, "%c=%[^\n]\n", string1, string2 ) == 2 )
	{
	  goto verifystart;
	}
        else if( sscanf( Line, "%[^=]=%[^\n]\n", string1, string2 ) == 2 )
	{
	  verifystart:;
	  nH = &H;
	  LenPrefix = strlen( string2 );
	  for( i = 0; i < LenPrefix; i ++ )
	  {
	    switch( string2[i] )
	    {
	      case '0':
	        if( nH->P0 != NULL )
		{
		  nH = nH->P0;
		}
	        break;
	      case '1':
	        if( nH->P1 != NULL )
		{
		  nH = nH->P1;
		}
	        break;
	      default:
	        break;
	    }
	  }
	  if( nH->Value != NULL )
	  {
	    if( memcmp( nH->Value, string1, strlen( nH->Value ) ) != 0 )
	    {
	      printf( "LoadEPG: Error, huffman prefix value '%s' not equal to '%s'", nH->Value, string1 );
	    }
	  }
	  else
	  {
	    printf( "LoadEPG: Error, huffman prefix value is not exists for \"%s\"=%s", string1, string2 );
	  }
        }
      }
    }
    fclose( FileDict );
  }
  free( FileName );
  return 1;
}
#endif
#if 0
bool cTaskLoadepg::ReadFileThemes( void )
{
  char *FileName;
  FILE *FileThemes;
  char *Line;
  char Buffer[256];
  asprintf( &FileName, "%s/%s", "conf", "sky_uk.dict" );
  FileThemes = fopen( FileName, "r" );
  if( FileThemes == NULL )
  {
    esyslog( "LoadEPG: Error opening file '%s'. %s", FileName, strerror( errno ) );
    free( FileName );
    return false;
  }
  else
  {
    int id = 0;
    char string1[256];
    char string2[256];
    while( ( Line = fgets( Buffer, sizeof( Buffer ), FileThemes ) ) != NULL )
    {
      memset( string1, 0, sizeof( string1 ) );
      memset( string2, 0, sizeof( string2 ) );
      if( ! isempty( Line ) )
      {
	sTheme *T = ( lThemes + id );
        if( sscanf( Line, "%[^=] =%[^\n] ", string1, string2 ) == 2 )
	{
	  snprintf( ( char * ) T->Name, 255, "%s", string2 );
	}
	else
	{
	  T->Name[0] = '\0';
	}
	id ++;
      }
    }
    fclose( FileThemes );
  }
  free( FileName );
  return true;

#endif

void clean_string( unsigned char *String )
{
  unsigned char *Src;
  unsigned char *Dst;
  int Spaces;
  int pC;
  Src = String;
  Dst = String;
  Spaces = 0;
  pC = 0;
  while( *Src )
  {
    // corrections
    if( *Src == 0x8c ) // iso-8859-2 LATIN CAPITAL LETTER S WITH ACUTE
    {
      *Src = 0xa6;
    }
    if( *Src == 0x8f ) // iso-8859-2 LATIN CAPITAL LETTER Z WITH ACUTE
    {
      *Src = 0xac;
    }
    
    if( *Src < 0x20 )
    {
      *Src = 0x20;
    }
    if( *Src == 0x20 )
    {
      Spaces ++;
      if( pC == 0 )
      {
        Spaces ++;
      }
    }
    else
    {
      Spaces = 0;
    }
    if( Spaces < 2 )
    {
      *Dst = *Src;
      *Dst ++;
      pC ++;
    }
    *Src ++;
  }
  if( Spaces > 0 )
  {
    Dst --;
    *Dst = 0;
  }
  else
  {
    *Dst = 0;
  }
}

/* No huffman is a5 */
/* Has a lot of 1f ff ff */
/* Similar to C1 */
void process_epg_test_a5_a6_a7( uint8_t *Data, int Length )
{
	int i, n;
	int tmp;
	printf("epg_test: TODO\n");  
	printf("MATCHA567 ");
	for(n = 0; n < 0x1c; n++) {
		printf("%02x ", Data[n]);
		if ((n % 32) == 31) {
			printf("\n");
		}
	}
	printf("\n");
	if (1) {
//		int satMJD = ( Data[3] << 8 ) | Data[4];
//		int satH = BcdToInt( Data[5] );
//		int satM = BcdToInt( Data[6] );
//		int satS = BcdToInt( Data[7] );
//		int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
//		for(n = 0; n < 0x46; n++) {
//			tmp = decode_huffman_code(&Data[n + 4], 0x46 - n, buffer_for_decode);
//			printf("Title:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
//			//tmp = Data[n] + n;
//			//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
//		}
//		printf("\n");
	/* Offset i == 11 seems to be good */
		for(n = 0; n < 0xa; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
		int p1 = 0xa;
		while( p1 < Length ) {
			switch (Data[p1 + 4]) {
			case 0xbc:
				for(n = 0; n < 4; n++) {
					printf("%02x ", Data[p1 + n]);
					if ((n % 32) == 31) {
						printf("\n");
					}
				}
				printf("\n");
				tmp = Data[p1 + 5];
				printf("%02x %02x\n", Data[p1 + 4], tmp);
				for(n = 0; n < tmp; n++) {
					printf("%02x ", Data[p1 + n + 6]);
					if ((n % 9) == 8) {
						printf("\n");
					}
				}
				printf("\n");
				tmp =
				p1 = p1 + tmp + 6;
				break;
			default:
				printf("ERROR A5 A6 A7 0x%x\n", Data[p1]);
				exit(0);
				break;
			}
//		        unsigned short int Sid = ( Data[p1] << 8 ) | Data[p1 + 1];
//		        unsigned short int Info = Data[p1 + 2];
//		        unsigned short int ChannelId = ( Data[p1 + 3] << 8 ) | Data[p1 + 4];
//			unsigned short int SkyNumber = ( Data[p1 + 5] << 8 ) | Data[p1 + 6];
			/* FIXME: JCD Not really sure what this SkyNumber2 is. */
//			printf( "Sid2 = 0x%x, ChannelId = 0x%x, Info = 0x%x, SkyNumber2 = 0x%x , %d\n", Sid, ChannelId, Info, SkyNumber, SkyNumber );
			/* Offset i == 11 seems to be good */
		}
	}
}

/* No huffman codes */
/* No huffman codes */
/* Found OS-BOOT.BOOT     AMSTRAD, so assume this is OS boot loader */
void process_epg_test_b5( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	int tmp;
	printf("epg_test: TODO\n");  
		printf("MATCHB5 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
	//  if ((Data[0x12] == 0) && (Data[0x13] == 0) )
	if (1) {
		int satMJD = ( Data[3] << 8 ) | Data[4];
		int satH = BcdToInt( Data[5] );
		int satM = BcdToInt( Data[6] );
		int satS = BcdToInt( Data[7] );
		int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
//		for(n = 0; n < 0x400; n++) {
//			tmp = decode_huffman_code(&Data[n + 4], 0x20, buffer_for_decode);
//			printf("Title:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
			//tmp = Data[n] + n;
			//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
//		}
		/* Offset i == 11 seems to be good */
		int p1 = 0x17;
		while( p1 < Length ) {
			int DescriptorTag = Data[p1];
			int DescriptorLength = Data[p1+1];
			int HuffLength = Data[p1+3];
			int SatelliteTimeOffsetPolarity;
			int SatelliteTimeOffsetH;
			int SatelliteTimeOffsetM;
			printf("\nDescriptorTag = 0x%x\n", DescriptorTag);
			printf("\nDescriptorLength = 0x%x\n", DescriptorLength);
			printf("\nHuffLength = 0x%x\n", HuffLength);
			switch( DescriptorTag ) {
			case 0xb9:
				for(n = 0; n < DescriptorLength; n++) {
					printf("%02x ", Data[p1 + n]);
					if ((n % 32) == 31) {
						printf("\n");
					}
				}
				printf("\n");
	/* Offset i == 11 seems to be good */
//		tmp = decode_huffman_code(&Data[p1 + 4], HuffLength, buffer_for_decode);
//		printf("Title:%d:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);


//		for(n = 0; n < HuffLength; n++) {
//			printf("%02x ", Data[p1 + 11 + n]);
//			if ((n % 32) == 31) {
//				printf("\n");
//			}
	/* Offset i == 11 seems to be good */
				break;
			default:
				printf( "ERROR 0x%02x\n", DescriptorTag );
				return;
				break;
			}
			p1 += ( DescriptorLength + 4 );
			DescriptorsLoopLength -= ( DescriptorLength + 2 );
		}
	}
}


/* No huffman codes */
/* Found OS-BOOT.BOOT     AMSTRAD, so assume this is OS boot loader */
/* Found OS-BOOT.BOOT     SAMSUNG, so assume this is OS boot loader */
/* Found OS-BOOT.BOOT     TVCOM, so assume this is OS boot loader */
/* Found OS-BOOT.BOOT     PACE, so assume this is OS boot loader */
/* The image is COMP compressed, and the last section has SIGN on the end */
void process_epg_test_b6( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	int tmp;
	printf("epg_test: TODO\n");  
		printf("MATCHB6 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
//  if ((Data[0x12] == 0) && (Data[0x13] == 0) )
  if (0)
  {
    int satMJD = ( Data[3] << 8 ) | Data[4];
    int satH = BcdToInt( Data[5] );
    int satM = BcdToInt( Data[6] );
    int satS = BcdToInt( Data[7] );
    int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
		for(n = 0; n < 0x400; n++) {
			tmp = decode_huffman_code(&Data[n + 4], 0x20, buffer_for_decode);
			printf("Title:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
			//tmp = Data[n] + n;
			//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
		}
	/* Offset i == 11 seems to be good */
//    int p1 = 0x46;
    int p1 = 0x24;
    while( p1 < Length )
    {
      int DescriptorTag = Data[p1];
      int DescriptorLength = Data[p1+1];
      int HuffLength = Data[p1+3];
      int SatelliteTimeOffsetPolarity;
      int SatelliteTimeOffsetH;
      int SatelliteTimeOffsetM;
	printf("\nDescriptorTag = 0x%x\n", DescriptorTag);
	printf("\nDescriptorLength = 0x%x\n", DescriptorLength);
	printf("\nHuffLength = 0x%x\n", HuffLength);
      switch( DescriptorTag )
      {
        case 0xb0:
		for(n = 0; n < 11; n++) {
			printf("%02x ", Data[p1 + n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
		tmp = decode_huffman_code(&Data[p1 + 4], HuffLength, buffer_for_decode);
		printf("Title:%d:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);


//		for(n = 0; n < HuffLength; n++) {
//			printf("%02x ", Data[p1 + 11 + n]);
//			if ((n % 32) == 31) {
//				printf("\n");
//			}
	/* Offset i == 11 seems to be good */
		break;
	default:
	  //fprintf( stderr, "0x%02x\n", DescriptorTag );
	  break;
      }
      p1 += ( DescriptorLength + 4 );
      DescriptorsLoopLength -= ( DescriptorLength + 2 );
    }
  }
}

/* C2: No huffman codes, Small SI size. No pattern. */
/* MATCHC2 c2 f0 57 a6 44 c3 00 00 04 cf ff 04 93 09 60 09 61 09 63 bd 41 00 00 04 00 3c 90 3a */
/* Looks like something to do with encryption. 0961, 0963 are CAIDs. */
void process_epg_test_c2( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	int tmp;
	printf("epg_test: TODO\n");  
		printf("MATCHC2 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
//  if ((Data[0x12] == 0) && (Data[0x13] == 0) )
  if (0)
  {
    int satMJD = ( Data[3] << 8 ) | Data[4];
    int satH = BcdToInt( Data[5] );
    int satM = BcdToInt( Data[6] );
    int satS = BcdToInt( Data[7] );
    int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
		for(n = 0; n < 0x46; n++) {
			tmp = decode_huffman_code(&Data[n + 4], 0x46 - n, buffer_for_decode);
			printf("Title:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
			//tmp = Data[n] + n;
			//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
//    int p1 = 0x46;
    int p1 = 0x24;
    while( p1 < Length )
    {
      int DescriptorTag = Data[p1];
      int DescriptorLength = Data[p1+1];
      int HuffLength = Data[p1+3];
      int SatelliteTimeOffsetPolarity;
      int SatelliteTimeOffsetH;
      int SatelliteTimeOffsetM;
	printf("\nDescriptorTag = 0x%x\n", DescriptorTag);
	printf("\nDescriptorLength = 0x%x\n", DescriptorLength);
	printf("\nHuffLength = 0x%x\n", HuffLength);
      switch( DescriptorTag )
      {
        case 0xb0:
		for(n = 0; n < 11; n++) {
			printf("%02x ", Data[p1 + n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
		tmp = decode_huffman_code(&Data[p1 + 4], HuffLength, buffer_for_decode);
		printf("Title:%d:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);


//		for(n = 0; n < HuffLength; n++) {
//			printf("%02x ", Data[p1 + 11 + n]);
//			if ((n % 32) == 31) {
//				printf("\n");
//			}
	/* Offset i == 11 seems to be good */
		break;
	default:
	  //fprintf( stderr, "0x%02x\n", DescriptorTag );
	  break;
      }
      p1 += ( DescriptorLength + 4 );
      DescriptorsLoopLength -= ( DescriptorLength + 2 );
    }
  }
//  Filters[FilterId].Step = 2;
}

/* C1: No huffman codes, but has repeated pattern data of ff ff .... ff ff. Large SI size. */
void process_epg_test_c1( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	int tmp;
	printf("epg_test: TODO\n");  
		printf("MATCHC1 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
//  if ((Data[0x12] == 0) && (Data[0x13] == 0) )
  if (1) {
    int satMJD = ( Data[3] << 8 ) | Data[4];
    int satH = BcdToInt( Data[5] );
    int satM = BcdToInt( Data[6] );
    int satS = BcdToInt( Data[7] );
    int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
//		for(n = 0; n < 0x46; n++) {
//			tmp = decode_huffman_code(&Data[n + 4], 0x46 - n, buffer_for_decode);
//			printf("Title:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
//			//tmp = Data[n] + n;
//			//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
//		}
		printf("\n");
	/* Offset i == 11 seems to be good */
//    int p1 = 0x46;
	int p1 = 0x8;
	while( p1 < Length ) {
		for(n = 0; n < 9; n++) {
			printf("%02x ", Data[p1 + n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	        unsigned short int Sid = ( Data[p1] << 8 ) | Data[p1 + 1];
	        unsigned short int Info = Data[p1 + 2];
	        unsigned short int ChannelId = ( Data[p1 + 3] << 8 ) | Data[p1 + 4];
	        unsigned short int SkyNumber = ( Data[p1 + 5] << 8 ) | Data[p1 + 6];
		/* FIXME: JCD Not really sure what this SkyNumber2 is. */
	        printf( "Sid2 = 0x%x, ChannelId = 0x%x, Info = 0x%x, SkyNumber2 = 0x%x , %d\n", Sid, ChannelId, Info, SkyNumber, SkyNumber );
	/* Offset i == 11 seems to be good */
//		tmp = decode_huffman_code(&Data[p1 + 4], HuffLength, buffer_for_decode);
//		printf("Title:%d:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);


//		for(n = 0; n < HuffLength; n++) {
//			printf("%02x ", Data[p1 + 11 + n]);
//			if ((n % 32) == 31) {
//				printf("\n");
//			}
	/* Offset i == 11 seems to be good */
	p1 = p1 + 9;
	}
//      p1 += ( DescriptorLength + 4 );
//      DescriptorsLoopLength -= ( DescriptorLength + 2 );
  }
//  Filters[FilterId].Step = 2;
}


/* C0 contains huffman coded strings using multiple sections added together */
void process_epg_test_c0( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	int tmp;
	int id;
	uint32_t offset;
	uint32_t total_length;
	int huff_length;
	uint8_t *data2;
	uint16_t ChannelId;
	uint16_t MjdTime;
	uint16_t EventId;
	printf("epg_test: TODO Length=0x%x\n", Length);  
		printf("MATCHC0 ");
		for(n = 0; n < 0x28; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");

	printf("Offset 0x01: %x\n", Data[1]);
	printf("Offset 0x03: %x\n", Data[3]);
	id = Data[4];
	printf("Offset 0x04 (Unique ID): %x\n", id);
	printf("Offset 0x05: %x\n", Data[5]);
	printf("Offset 0x15: %x\n", Data[0x15]);
	offset = Data[0x10] << 24 | Data[0x11] << 16 | Data[0x12] << 8 | Data[0x13];
	total_length = Data[0x14] << 24 | Data[0x15] << 16 | Data[0x16] << 8 | Data[0x17];
	printf("offset (0x10): %x\n", offset);
	printf("total length (0x14): %x\n", total_length);
	printf("Offset 0x18 (payload type when offset == 0)): %x\n", Data[0x18]);

	if (Data[3] == 1) {
		if (offset == 0) {
			tmp = Length - 0x18;
			section_c0[id].offset = tmp;
			section_c0[id].total_length = total_length;
			section_c0[id].summary = calloc( 1, total_length);
			section_c0[id].summary_length = total_length;
			memcpy( &section_c0[id].summary[0], &Data[0x18], tmp);
			printf("ID:0x%x, offset = 0x%x, len=0x%x, total=0x%x\n",
				id, offset, tmp, total_length);
		}
		if ((offset != 0) && (section_c0[id].total_length != 0)) {
			if (offset == section_c0[id].offset) {
				tmp = Length - 0x18;
				memcpy( &section_c0[id].summary[offset], &Data[0x18], tmp);
				section_c0[id].offset += tmp;
				printf("ID:0x%x, offset = 0x%x, len=0x%x, total=0x%x\n",
					id, offset, tmp, total_length);
			} else {
				printf("ID FAILED:0x%x, offset = 0x%x, len=0x%x, total=0x%x\n",
					id, offset, tmp, total_length);
			}
		}
		if ((section_c0[id].total_length != 0) && (section_c0[id].offset == section_c0[id].total_length)) {
    			int DescriptorsLoopLength = section_c0[id].total_length;
			data2 = section_c0[id].summary;
			for(n = 0; n < 0x40; n++) {
				printf("%02x ", data2[n]);
				if ((n % 32) == 31) {
					printf("\n");
				}
			}
//printf("ID:0x%x FINISHED\n", id);
//			huff_length = Data[0x27];
//			printf("huff_length = 0x%x\n", huff_length);
//		
//			for(n = 0x18; n < Length; n++) {
//				tmp = decode_huffman_code(&Data[n], huff_length, buffer_for_decode);
//				printf("TitleC0:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
				//tmp = Data[n] + n;
				//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
//			}
			int p1 = 0x0c;
			int counter = 0;
			while( p1 < section_c0[id].total_length ) {
				int DescriptorLength = data2[p1 + 1];
				if (DescriptorLength == 0) {
					printf("Skipping 4\n");
					//p1 += 4;
				}
				int Unknown1 = ( data2[p1 - 4] << 8 ) | data2[p1 - 3];
				int EventId = ( data2[p1 - 2] << 8 ) | data2[p1 - 1];
				int DescriptorTag = data2[p1];
				DescriptorLength = data2[p1 + 1];
				int HuffTag = data2[p1 + 2];
				int HuffLength = data2[p1 + 3];
				printf("\nUnknown = 0x%x\n", Unknown1);
				printf("EventId = 0x%x\n", EventId);
				printf("DescriptorTag = 0x%x\n", DescriptorTag);
				printf("DescriptorLength = 0x%x\n", DescriptorLength);
				printf("HuffTag = 0x%x\n", HuffTag);
				printf("HuffLength = 0x%x\n", HuffLength);
				printf("p1= 0x%x\n", p1);
				switch( HuffTag ) {
				case 0xb9:
					for(n = -4; n < DescriptorLength + 4; n++) {
						printf("%02x ", data2[p1 + n]);
						if ((n % 32) == 31) {
							printf("\n");
						}
					}
					printf("\n");
					tmp = decode_huffman_code(&data2[p1 + 4], HuffLength, buffer_for_decode);
					printf("TitleC0:%d:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
					break;
				case 0xa8:
				case 0xa9:
				case 0xaa:
				case 0xab:
					/* Have to work out what really determine the space between a 0xa8-0xab and a 0xb9.*/
					printf("MATCHC01:");
					for(n = -7; n < 0x0a + 8; n++) {
						printf("%02x ", data2[p1 + n]);
						if ((n % 32) == 31) {
							printf("\n");
						}
					}
					printf("\n");
					p1 += 0x02;
					ChannelId = ( data2[p1 + 3] << 8 ) | data2[p1 + 4];
					MjdTime = ( ( data2[p1 + 8] << 8 ) | data2[p1 + 9] );
					printf("MATCHC01: ChannelID = 0x%x, MjdTime = 0x%x\n", ChannelId, MjdTime);
					p1 += 0x08;
					DescriptorLength = 0;
					break;
				case 0xd0:
					printf( "d0-Descriptor Tag=0x%02x, HuffTag=0x%x, offset=0x%x\n", DescriptorTag, HuffTag, p1 );
					tmp = DescriptorLength + 4;
					if (tmp + p1 > section_c0[id].total_length) {
						printf("Overflowed\n");
						tmp = section_c0[id].total_length - p1;
					}
					for(n = -4; n < tmp; n++) {
						printf("%02x ", data2[p1 + n]);
						if ((n % 16) == 15) {
							for(i = 0; i < 16; i++) {
								tmp = data2[p1 -16 - 4 + n + i];
								if (tmp < 32 || tmp > 127) {
									tmp='.';
								}
								printf("%c ", tmp);
							} 
							printf("\n");
						}
					}
					printf("\n");
					for(n = 0; n < 0x46; n++) {
						tmp = decode_huffman_code(&data2[p1 + n], 0x46, buffer_for_decode);
						printf("TitleC02:0x%x:%d:%s:::::::%s\n", n, tmp, DecodeText, DecodeErrorText);
						//tmp = Data[n] + n;
						//printf("MATCH n = 0x%x, tmp = 0x%x\n", n, tmp);
					}
					printf("\n");
	/* Offset i == 11 seems to be good */
	  				break;
				default:
					printf( "C0-Descriptor unknown Tag=0x%02x, HuffTag=0x%x, offset=0x%x\n", DescriptorTag, HuffTag, p1 );
					tmp = DescriptorLength + 4;
					if (tmp + p1 > section_c0[id].total_length) {
						printf("Overflowed\n");
						tmp = section_c0[id].total_length - p1;
					}
					for(n = -4; n < tmp; n++) {
						printf("%02x ", data2[p1 + n]);
						if ((n % 32) == 31) {
							printf("\n");
						}
					}
					printf("\n");
	  				break;
				}
				p1 += ( DescriptorLength + 4 );
				DescriptorsLoopLength -= ( DescriptorLength + 2 );
				counter++;
				//if (counter == 7) p1 += 0x0e;
			}
			//exit(0);
		}
	}
}

// SKYBOX Stuff {{{
// cTaskLoadepg::SupplementChannelsSKYBOX {{{
/* Not multiple sections added together. Example:
46 f2 fb 07 e1 c1 00 00 00 02 ff 
 1c 3e fe 80 <- First one.
 a5 <- Length
48 0b 01  05 42 53 6b 79 42  03 51 56 43
49 07 ff 47 42 52 49 52 4c
5f 04 00 00 00 02
b2 87 1d 01 30 22 af d5 50 b2 86 ab 8c 21 f4 2a 40 da ae 30 24 d5 71 82 27 d6 60 d8 2e 63 8f 57 18 29 47 b9 bc bb fd 35 5c 60 a5 aa f9 db 6e 85 b7 02 ab 8c 14 a3 0d 66 b5 3e 53 57 18 29 47 b8 15 5c 60 48 1a ae 30 24 df d5 f3 4b 41 6a 7e 29 78 fc 35 5c 60 8b a4 8e d7 4e ab 8c 21 f5 5c 61 49 a5 b4 65 f2 d8 72 5e 7c 2c 69 a5 37 35 71 83 67 a7 70 47 4e 83 fa 59 32 8e ae ae 30 69 f4 77 09 e6 06 8e d9 29 76 ff ed
 1c 3f fe 80 <- Next one
 aa <- Length
...
*/
/* This is really process_sdt_actual, but need to merge it */
void process_epg_suppliment_channels(uint8_t *Data, int Length)
{
	int n;

	printf("MATCHSUP0 ");
	for(n = 0; n < Length; n++) {
		printf("%02x ", Data[n]);
		if ((n % 32) == 31) {
			printf("\n");
		}
	}
	printf("\n");
#if 0
	/* Offset i == 11 seems to be good */
    if (!EndBAT) {
	return;
    }

    if (EndSDT) {
	//Filters[FilterId].Step = 2;
	printf("endsdt");
	return;
    }

//    SI::SDT sdt(Data, false);
//    if (!sdt.CheckCRCAndParse())
//	return;

    SI::SDT::Service SiSdtService;
    for (SI::Loop::Iterator it; sdt.serviceLoop.getNext(SiSdtService, it);) {

	sChannel Key, *C;
	Key.ChannelId = Key.Sid = SiSdtService.getServiceId();
	Key.Nid = sdt.getOriginalNetworkId();
	Key.Tid = sdt.getTransportStreamId();
	C = (sChannel *) bsearch(&Key, lChannels, nChannels, sizeof(sChannel), &bsearchChannelBySid);

	if (firstSDTChannel == NULL) {
	    firstSDTChannel = C;
	} else if (firstSDTChannel == C) {
	    if (nChannelUpdates == 0) {
		EndSDT = true;
	    } else
		nChannelUpdates = 0;
	}

	SI::Descriptor * d;
	for (SI::Loop::Iterator it2; (d = SiSdtService.serviceDescriptors.getNext(it2));) {
	    switch (d->getDescriptorTag()) {
		case 0x48:   //SI::ServiceDescriptorTag:
		    {
			SI::ServiceDescriptor * sd = (SI::ServiceDescriptor *) d;
			switch (sd->getServiceType()) {
			    case 0x01:	// digital television service
			    case 0x02:	// digital radio sound service
			    case 0x04:	// NVOD reference service
			    case 0x05:	// NVOD time-shifted service
				{
				    char NameBuf[1024];
				    char ShortNameBuf[1024];
				    char ProviderNameBuf[1024];
				    log_message(TRACE, "B %02x %x-%x %x-%x %x-%x",
					    sd->getServiceType(), Key.Nid, lChannels[10].Nid, Key.Tid, lChannels[10].Tid, Key.Sid, lChannels[10].Sid);
				    sd->serviceName.getText(NameBuf, ShortNameBuf, sizeof(NameBuf), sizeof(ShortNameBuf));
				    char *pn = compactspace(NameBuf);
				    sd->providerName.getText(ProviderNameBuf, sizeof(ProviderNameBuf));
				    char *provname = compactspace(ProviderNameBuf);
				    if (C) {
					if (C->name == NULL) {
					    asprintf(&C->name, "%s", pn);
					    asprintf(&C->providername, "%s", provname);
					}
				    }
				}
				break;
			    default:
				break;
			}
		    }
		    break;
		case 0x5d:  //SI::MultilingualServiceNameDescriptorTag:
		    {
			if (C == NULL)
			    break;
			SI::MultilingualServiceNameDescriptor * md = (SI::MultilingualServiceNameDescriptor *) d;
			SI::MultilingualServiceNameDescriptor::Name n;
			for (SI::Loop::Iterator it2; (md->nameLoop.getNext(n, it2));) {
			    // languageCode char[4]
			    // name String
			    if (strncmp(n.languageCode, "aka", 3) == 0) {
				if (C->shortname == NULL) {
				    char b[100];
				    n.name.getText(b, sizeof(b));
				    C->shortname = strdup(b);
				    nChannelUpdates++;
				}
			    } else {
				if (!C->IsNameUpdated) {
				    if (C->name) {
					free(C->name);
					C->name = NULL;
				    }
				    char b[100];
				    n.name.getText(b, sizeof(b));
				    C->name = strdup(b);
				    C->IsNameUpdated = true;
				}
			    }
			}
		    }
		    break;
		default:
		    break;
	    }
	}
    }
#endif
}

void process_epg_time_offset( uint8_t *Data, int Length )
{
	uint8_t SatelliteCountryCode[4];
	int i, n;
	printf("Time_offset: TODO\n");  
		printf("MATCHTO0 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
  if( Data[0] == 0x73 )
  {
    int satMJD = ( Data[3] << 8 ) | Data[4];
    int satH = BcdToInt( Data[5] );
    int satM = BcdToInt( Data[6] );
    int satS = BcdToInt( Data[7] );
    int DescriptorsLoopLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
    int p1 = 10;
    while( DescriptorsLoopLength > 0 )
    {
      int DescriptorTag = Data[p1];
      int DescriptorLength = Data[p1+1];
      int SatelliteCountryRegionId;
      int SatelliteTimeOffsetPolarity;
      int SatelliteTimeOffsetH;
      int SatelliteTimeOffsetM;
	printf("\nDescriptorLength = 0x%x\n", DescriptorLength);
      switch( DescriptorTag )
      {
        case 0x58:
	  for( i = 0; i < 3; i ++ )
	  {
	    SatelliteCountryCode[i] = Data[p1+2+i];
	  }
	  SatelliteCountryCode[3] = '\0';
	  clean_string(SatelliteCountryCode);
	  SatelliteCountryRegionId = ( Data[p1+5] & 0xfc ) >> 6;
	  SatelliteTimeOffsetPolarity = ( Data[p1+5] & 0x01 );
	  SatelliteTimeOffsetH = BcdToInt( Data[p1+6] );
	  SatelliteTimeOffsetM = BcdToInt( Data[p1+7] );
	  if( SatelliteTimeOffsetPolarity == 1 )
	  {
	    SatelliteTimeOffset = 0 - ( SatelliteTimeOffsetH * 3600 );
	  }
	  else
	  {
	    SatelliteTimeOffset = SatelliteTimeOffsetH * 3600;
	  }
	  EpgTimeOffset = ( LocalTimeOffset - SatelliteTimeOffset );
	  printf("LoadEPG: Satellite Time Offset=[UTC]%+i", SatelliteTimeOffset / 3600);
	  printf("LoadEPG: Epg Time Offset=%+i seconds", EpgTimeOffset);
	  if( 1 )
	  {
	    printf( "LoadEPG: Satellite Time UTC: %s %02i:%02i:%02i", GetStringMJD( satMJD ), satH, satM, satS );
	    printf( "LoadEPG: Satellite CountryCode=%s", SatelliteCountryCode );
	    printf( "LoadEPG: Satellite CountryRegionId=%i", SatelliteCountryRegionId );
	    printf( "LoadEPG: Satellite LocalTimeOffsetPolarity=%i", SatelliteTimeOffsetPolarity );
	    printf( "LoadEPG: Satellite LocalTimeOffset=%02i:%02i", SatelliteTimeOffsetH, SatelliteTimeOffsetM );
	  }
	  break;
	default:
	  //fprintf( stderr, "0x%02x\n", DescriptorTag );
	  break;
      }
      p1 += ( DescriptorLength + 2 );
      DescriptorsLoopLength -= ( DescriptorLength + 2 );
    }
  }
//  Filters[FilterId].Step = 2;
}

int process_epg_channels( unsigned char *Data, int Length )
{
	int i, ii, n;
	unsigned char SectionNumber = Data[6];
	unsigned char LastSectionNumber = Data[7];
	printf("MATCHCH0 ");
	for(n = 0; n < 0x1c; n++) {
		printf("%02x ", Data[n]);
		if ((n % 32) == 31) {
			printf("\n");
		}
	}
	printf("\n");
	/* Offset i == 11 seems to be good */

	printf("Channels: Data[0] = 0x%x, SectionNumber = 0x%x, LastSectionNumber = 0x%x.\n", Data[0], SectionNumber, LastSectionNumber);  
	if( SectionNumber == 0x00 && nBouquets == 0 ) {
		return 0;
	}
  
	// Table BAT
	if( Data[0] == 0x4a ) {
		if( EndBAT ) {
			//Filters[FilterId].Step = 2;
			return 0;
		}
		printf("Channels: Bouquets\n");
		unsigned short int BouquetId = ( Data[3] << 8 ) | Data[4];
		int BouquetDescriptorsLength = ( ( Data[8] & 0x0f ) << 8 ) | Data[9];
		int TransportStreamLoopLength = ( ( Data[BouquetDescriptorsLength+10] & 0x0f ) << 8 ) | Data[BouquetDescriptorsLength+11];
		int p1 = ( BouquetDescriptorsLength + 12 );
		printf("Channels: BouquetID = 0x%x, BouquetDescLength = 0x%x, TransportStreamLoopLen = 0x%x, p1 = 0x%x\n", BouquetId, BouquetDescriptorsLength, TransportStreamLoopLength, p1);
		while( TransportStreamLoopLength > 0 ) {
			unsigned short int Tid = ( Data[p1] << 8 ) | Data[p1+1];
			unsigned short int Nid = ( Data[p1+2] << 8 ) | Data[p1+3];
			int TransportDescriptorsLength = ( ( Data[p1+4] & 0x0f ) << 8 ) | Data[p1+5];
			int p2 = ( p1 + 6 );
			p1 += ( TransportDescriptorsLength + 6 );
			TransportStreamLoopLength -= ( TransportDescriptorsLength + 6 );
			while( TransportDescriptorsLength > 0 ) {
				unsigned char DescriptorTag = Data[p2];
				int DescriptorLength = Data[p2+1];
				int p3 = ( p2 + 2 );
				p2 += ( DescriptorLength + 2 );
				TransportDescriptorsLength -= ( DescriptorLength + 2 );
				switch( DescriptorTag ) {
				case 0xb1:
					printf( "Found Tag 0x%02x\n", DescriptorTag );
					p3 += 2;
					DescriptorLength -= 2;
					while( DescriptorLength > 0 ) {
						// 0x01 = Video Channel
						// 0x02 = Audio Channel
						// 0x05 = Other Channel
						//if( Data[p3+2] == 0x01 || Data[p3+2] == 0x02 || Data[p3+2] == 0x05 )
						//{
						uint16_t Sid = ( Data[p3] << 8 ) | Data[p3 + 1];
						uint16_t Info = Data[p3 + 2];
						uint16_t ChannelId = ( Data[p3 + 3] << 8 ) | Data[p3 + 4];
						uint16_t SkyNumber = ( Data[p3 + 5] << 8 ) | Data[p3 + 6];
						printf( "Sid = 0x%x, ChannelId = 0x%x, Info = 0x%x, SkyNumber = 0x%x , %d\n", Sid, ChannelId, Info, SkyNumber, SkyNumber );
						//if( SkyNumber > 100 && SkyNumber < 1000 )
						{
							if( ChannelId > 0 ) {
								struct channel_s Key, *C;
								Key.ChannelId = ChannelId;
								Key.Nid = Nid;
								Key.Tid = Tid;
								Key.Sid = Sid;
								printf("nChannels=0x%x, ChannelID=0x%x, Nid=0x%x, Tid=0x%x, Sid=0x%x, C=%p\n", nChannels, ChannelId, Nid, Tid, Sid, C);
								if (channels_all[ChannelId] == 0xffff) {
									channels_all[ChannelId] = nChannels;
									nChannels ++;
									if( nChannels >= MAX_CHANNELS ) {
										printf( "Channels: Error, channels found more than %i", MAX_CHANNELS );
										return 0;
									}
								}
								C = &lChannels[channels_all[ChannelId]];
								C->ChannelId = ChannelId;
								C->Nid = Nid;
								C->Tid = Tid;
								C->Sid = Sid;
								if (!(C->SkyNumber1) || (C->SkyNumber1 == SkyNumber))
									C->SkyNumber1 = SkyNumber;
								else
									C->SkyNumber2 = SkyNumber;
								C->info = Info;
								C->pData = 0;
								C->lenData = 0;
								C->IsFound = 0;
								C->IsEpg = 1;
								//qsort( lChannels, nChannels, sizeof( struct channel_s ), &qsortChannelsByChID );
							}
						}
						//}
						p3 += 9;
						DescriptorLength -= 9;
					}
					break;
					default:
						printf( "Channels: Unknown Tag 0x%02x\n", DescriptorTag );
						break;
				}
			}
		}
		//	return 1; /* FIXME: JCD */
		struct bouquet_s *B;
		for( i = 0; i < nBouquets; i ++ ) {
			B = ( lBouquets + i );
			if( B->BouquetId == BouquetId ) {
				goto CheckBouquetSections;
			}
		}
		B = ( lBouquets + nBouquets );
		B->BouquetId = BouquetId;
		for( i = 0; i <= LastSectionNumber; i ++ ) {
			B->SectionNumber[i] = -1;
		}
		B->LastSectionNumber = LastSectionNumber;
		nBouquets ++;
		CheckBouquetSections:;
		B->SectionNumber[SectionNumber] = SectionNumber;
		EndBAT = 1;
		for( i = 0; i < nBouquets; i ++ ) {
			B = ( lBouquets + i );
			for( ii = 0; ii <= B->LastSectionNumber; ii ++ ) {
				if( B->SectionNumber[ii] == -1 ) {
					EndBAT = 0;
					break;
				}
			}
		}
	}
	return 1;
}


int process_epg_titles(uint8_t * Data, int Length) {
	uint16_t ChannelId;
	uint64_t MjdTime;
	uint16_t event_offset_word;
	uint64_t event_offset_time;
	uint16_t EventId;
	uint64_t start_time;
	uint64_t group_time;
	int	duration;
	int	theme_id;
	int Len1;
	int Len2;
	int p;
	int n;
	int tmp;
	struct channel_s *C;
	int found;
	struct tm tm1, *tm2;
	tm2 = &tm1;

		printf("MATCHT0 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	if (Length < 0x16) {
		printf("ERROR Title too short. Length=0x%04x\n", Length);
		return 1;
	}
	/* Offset i == 11 seems to be good */
	ChannelId = ( Data[3] << 8 ) | Data[4];
/* FIXME: JCD temp added */
	if (ChannelId != 0x540) return;
	MjdTime = ( ( Data[8] << 8 ) | Data[9] );
	group_time = ( ( MjdTime - 40587 ) * 86400 );
	tm2 = gmtime_r(&group_time, &tm1);
	printf("Titles: ChannelID = 0x%x, group_time = %lx, MjdTime = %04d-%02d-%02d %02d:%02d:%02d\n", ChannelId,
				group_time,
				tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday,
				tm1.tm_hour, tm1.tm_min, tm1.tm_sec);
	if( ChannelId > 0 ) {
		if (channels_all[ChannelId] == 0xffff) {
			channels_all[ChannelId] = nChannels;
			nChannels ++;
			if( nChannels >= MAX_CHANNELS ) {
				printf( "Titles: Error, channels found more than %i", MAX_CHANNELS );
				return 0;
			}
		}
		C = &lChannels[channels_all[ChannelId]];
		C->ChannelId = ChannelId;
		if( MjdTime > 0 ) {
			p = 10;
			loop1:;
			//sSummary *S = ( lSummaries + nSummaries );
			//S->ChannelId = ChannelId;
			//S->MjdTime = MjdTime;
			EventId = ( Data[p] << 8 ) | Data[p + 1];
			Len1 = ( ( Data[p + 2] & 0x0f ) << 8 ) | Data[p + 3];
			printf("Titles: ChannelID = 0x%x, EventID = 0x%x, Len1 = 0x%x\n", ChannelId, EventId, Len1);
			//if( Data[p + 4] != 0xb5 ) {
			//	printf("LoadEPG: Data error signature for titles Data[p+4] == 0x%x\n", Data[p + 4]);
			//	goto endloop1;
			//}
			printf("LoadEPG: Data signature for titles Data[p+4] == 0x%x\n", Data[p + 4]);
			if( Len1 > Length ) {
				printf("LoadEPG: Data error length for titles\n");
				goto endloop1;
			}
			p += 4;
			Len2 = Data[p + 1] - 7;
			printf("Titles: Len2 = 0x%x\n", Len2);
			/* This event_offset_word data is a 16bit unsigned integer. */
			/* Event start times can be less that MjdTime */
			/* If it is >0xc000 treat it as negative. */
			event_offset_word =  ( ( Data[p + 2] << 8 ) | ( Data[p + 3] ) );
			if (event_offset_word > 0xc000) {
				event_offset_time = (int16_t) event_offset_word;
			} else {
				event_offset_time = (uint16_t) event_offset_word;
			}
			event_offset_time = event_offset_time * 2;
			start_time = group_time + event_offset_time;
			duration = ( ( Data[p + 4] << 8 ) | ( Data[p + 5] ) );
			duration = duration * 2;
			theme_id = Data[p + 6];
					
			tm2 = gmtime_r(&start_time, &tm1);
			printf("Titles: ChannelID2 = 0x%x, event_offset_word = 0x%x, event_offset_time = 0x%lx, starttime=0x%lx, StartTime = %04d-%02d-%02d %02d:%02d:%02d, Duration = 0x%x, ThemeID = 0x%x\n",
				ChannelId,
				event_offset_word,
				event_offset_time,
				start_time,
				tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday,
				tm1.tm_hour, tm1.tm_min, tm1.tm_sec,
				duration, theme_id);
			for(n = 0; n < Len2; n++) {
				printf("%02x ", Data[p + 9 + n]);
				if ((n % 32) == 31) {
					printf("\n");
				}
			}
			printf("\n");
			tmp = decode_huffman_code(&Data[p + 9], Len2, buffer_for_decode);
			printf("Title:%d:%s:%s\n", tmp, DecodeText, DecodeErrorText);
			printf("ChannelID = 0x%x, EventID = 0x%x, %04d-%02d-%02d %02d:%02d:%02d, Len1 = 0x%x, Len2 = 0x%x TITLE %s\n", ChannelId, EventId,
				tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday,
				tm1.tm_hour, tm1.tm_min, tm1.tm_sec,
				Len1, Len2,
				DecodeText);
			if (!(C->events_count)) {
				C->events = calloc(1, sizeof(struct event_s));
				C->events[0].event_id = EventId;
				C->events[0].channel_id = ChannelId;
				C->events[0].start_time_title = start_time;
				C->events[0].duration_title = duration;
				C->events[0].theme_id = theme_id;
				C->events[0].prefix_len = 0; /* FIXME: JCD TODO */
				C->events[0].title_len = tmp;
				C->events[0].title = malloc(tmp + 1);
				memcpy(C->events[0].title, DecodeText, tmp);
				C->events[0].title[tmp] = 0;
				C->events_count = 1;
			} else {
				found = -1;
				for(n = 0; n < C->events_count; n++) {
					if (C->events[n].event_id == EventId) {
						found = n;
						break;
					}
				}
				if (found == -1) {
					found = C->events_count;
					C->events_count++;
					C->events = realloc( C->events, C->events_count * sizeof(struct event_s));
					/* Zero out fields that are not set in a moment */
					C->events[found].start_time_summary = 0;
					C->events[found].summary_len = 0;
					C->events[found].summary = NULL;
				}
				C->events[found].event_id = EventId;
				C->events[found].channel_id = ChannelId;
				C->events[found].start_time_title = start_time;
				C->events[found].duration_title = duration;
				C->events[found].theme_id = theme_id;
				C->events[found].prefix_len = 0; /* FIXME: JCD TODO */
				C->events[found].title_len = tmp;
				C->events[found].title = malloc(tmp + 1);
				memcpy(C->events[found].title, DecodeText, tmp);
				C->events[found].title[tmp] = 0;
			}

			p += Len1;
			if( p < Length ) {
				goto loop1;
			}
			endloop1:;
		}
	}
}

int process_epg_summary(uint8_t * Data, int Length) {
	uint16_t ChannelId;
	uint16_t MjdTime;
	uint16_t EventId;
	int Type;
	int Len1;
	int Len2;
	int p;
	int n;
	int tmp;
	struct channel_s *C;
	int found;
	struct tm tm1, *tm2;
	tm2 = &tm1;

		printf("MATCHS0 ");
		for(n = 0; n < 0x1c; n++) {
			printf("%02x ", Data[n]);
			if ((n % 32) == 31) {
				printf("\n");
			}
		}
		printf("\n");
	/* Offset i == 11 seems to be good */
	ChannelId = ( Data[3] << 8 ) | Data[4];
	if (ChannelId != 0x540) return;
	MjdTime = ( ( Data[8] << 8 ) | Data[9] );
	printf("Summary: ChannelID = 0x%x, MjdTime = 0x%x\n", ChannelId, MjdTime);
	if( ChannelId > 0 ) {
		if (channels_all[ChannelId] == 0xffff) {
			channels_all[ChannelId] = nChannels;
			nChannels ++;
			if( nChannels >= MAX_CHANNELS ) {
				printf( "Titles: Error, channels found more than %i", MAX_CHANNELS );
				return 0;
			}
		}
		C = &lChannels[channels_all[ChannelId]];
		C->ChannelId = ChannelId;
		if( MjdTime > 0 ) {
			p = 10;
			loop1:;
			//sSummary *S = ( lSummaries + nSummaries );
			//S->ChannelId = ChannelId;
			//S->MjdTime = MjdTime;
			EventId = ( Data[p] << 8 ) | Data[p+1];
			Type = Data[p + 2];
			if (Type != 0xb0) {
				printf("Summary: No 0xb0 found. Found 0x%x\n", Type);
				goto endloop1;
			}
			Len1 = Data[p + 3];
			printf("Summary: ChannelID = 0x%x, EventID = 0x%x, Len1 = 0x%x\n", ChannelId, EventId, Len1);
			if (Len1 < 4) {
				printf("Summary too short\n");
				p += Len1 + 4;
				goto reloop;
			}
			if( Data[p+4] != 0xb9 ) {
				printf("LoadEPG: Data error signature for summary\n");
				goto endloop1;
			}
			if( Len1 > Length ) {
				printf("LoadEPG: Data error length for summary\n");
				goto endloop1;
			}
			p += 4;
			Len2 = Data[p+1];
			printf("Summary: Len2 = 0x%x\n", Len2);
//			S->pData = pS;
//			S->lenData = Len2;
//			if( ( pS + Len2 + 2 ) > MAX_BUFFER_SIZE_SUMMARIES) {
//				printf("LoadEPG: Error, buffer overflow, summaries size more than %i bytes\n", MAX_BUFFER_SIZE_SUMMARIES);
//				IsError = true;
//				return;
//			}
//			memcpy( &bSummaries[pS], &Data[p+2], Len2 );
			for(n = 0; n < Len2; n++) {
				printf("%02x ", Data[p + 2 + n]);
				if ((n % 32) == 31) {
					printf("\n");
				}
			}
	printf("\n");
			tmp = decode_huffman_code(&Data[p + 2], Len2, buffer_for_decode);
			printf("Summary:%d:%s:%s\n", tmp, DecodeText, DecodeErrorText);
			printf("ChannelID = 0x%x, EventID = 0x%x, Len1 = 0x%x, Len2=0x%x SUMMARY %s\n", ChannelId, EventId, Len1, Len2, DecodeText);

			if (!(C->events_count)) {
				C->events = calloc(1, sizeof(struct event_s));
				C->events[0].event_id = EventId;
				C->events[0].channel_id = ChannelId;
				C->events[0].prefix_len = 0; /* FIXME: JCD TODO */
				C->events[0].summary_len = tmp;
				C->events[0].summary = malloc(tmp + 1);
				memcpy(C->events[0].summary, DecodeText, tmp);
				C->events[0].summary[tmp] = 0;
				C->events_count = 1;
			} else {
				found = -1;
				for(n = 0; n < C->events_count; n++) {
					if (C->events[n].event_id == EventId) {
						found = n;
						break;
					}
				}
				if (found == -1) {
					found = C->events_count;
					C->events_count++;
					C->events = realloc( C->events, C->events_count * sizeof(struct event_s));
					/* Zero out fields that are not set in a moment */
					C->events[found].start_time_title = 0;
					C->events[found].duration_title = 0;
					C->events[found].theme_id = 0;
					C->events[found].start_time_summary = 0;
					C->events[found].title_len = 0;
					C->events[found].title = NULL;
				}
				C->events[found].event_id = EventId;
				C->events[found].channel_id = ChannelId;
				C->events[found].prefix_len = 0; /* FIXME: JCD TODO */
				C->events[found].summary_len = tmp;
				C->events[found].summary = malloc(tmp + 1);
				memcpy(C->events[found].summary, DecodeText, tmp);
				C->events[found].summary[tmp] = 0;
			}
//			pS += ( Len2 + 1 );
			p += Len1;
//			nSummaries ++;
//			if( nSummaries >= MAX_SUMMARIES ) {
//				printf("LoadEPG: Error, summaries found more than %i\n", MAX_SUMMARIES);
//				IsError = true;
//				return;
//			}
			reloop:;
			if( p < Length ) {
				goto loop1;
			}
			endloop1:;
		}
	}
}

static void process_epg(struct demux_ts_s *this, int pid)
{
	struct program_s *program;
	int section_length;
	uint32_t crc32;
	uint32_t calc_crc32;
	uint8_t *buffer;
	int n;
	uint32_t	reserved1;
	uint32_t	pcr_pid;
	uint32_t	reserved2;
	uint32_t       program_info_length;
	uint32_t	stream_type;
	uint32_t	elementary_pid;
	uint32_t	es_info_length;
	uint32_t	desc_tag;
	uint32_t	desc_len;
	uint32_t	ca_system_id;
	uint32_t	ca_pid;
	uint32_t	offset;
	int		program_count;
	int		i;
	int		m;

#ifdef TS_PMT_LOG
  printf ("ts_demux: have all TS packets for the EPG section\n");
#endif
	program_count = this->pids[pid].program_count;
	program = &(this->programs[program_count]);
	buffer = this->pids[pid].section.whole_section;
	section_length = this->pids[pid].section.size;
	printf("buffer=%p, len=0x%x\n", buffer, section_length);
	printf("printing bytes=0x%x\n", section_length + 7);
	i = 0;
	for(n = 0; n < section_length + 7; n++) {
		printf("%02x ", buffer[n]);
		
		if ((n % 32) == 31) {
			for (m = i; m < (i + 32); m++) {
				if (buffer[m] > 31 && buffer[m] < 127) {
					printf("%c", buffer[m]);
				} else {
					printf(".", buffer[m]);
				}
			}
			i = n + 1;
			printf("\n");
		}
	}
	for (m = i; m < (n); m++) {
		if (buffer[m] > 31 && buffer[m] < 127) {
			printf("%c", buffer[m]);
		} else {
			printf(".", buffer[m]);
		}
	}
	printf("\n");

	crc32  = (uint32_t) buffer[section_length  - 4] << 24;
	crc32 |= (uint32_t) buffer[section_length  - 3] << 16;
	crc32 |= (uint32_t) buffer[section_length  - 2] << 8;
	crc32 |= (uint32_t) buffer[section_length  - 1] ;

	/* Check CRC. */
	calc_crc32 = demux_ts_compute_crc32(this,
		buffer,
		section_length - 4, 0xffffffff);
	if (crc32 != calc_crc32) {
		printf ("demux_ts: demux error! EPG CRC32 invalid: packet_crc32: %#.8x calc_crc32: %#.8x\n",
			crc32,calc_crc32);
		return;
	}
#ifdef TS_PMT_LOG
	printf ("demux_ts: EPG CRC32 ok: %#.8x\n", crc32);
#endif
	/* SKY BOX */	
	switch( buffer[0] ) {
//	case 0x73: /* Not seen */
//		printf("demux_ts: time: Unknown EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
//		process_epg_time_offset(buffer, section_length - 4);
//		break;
//	case 0x4a: /* In PID 0x11 */
//		printf("demux_ts: chan: Unknown EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
//		process_epg_channels(buffer, section_length - 4);
//		break;
	case 0xa0:
	case 0xa1:
	case 0xa2:
	case 0xa3:
	case 0xa4:
	case 0xb0:
		process_epg_titles(buffer, section_length - 4);
		break;
	case 0xa8: /* FIXME: Needs more processing, this is a multi-segment record. */
	case 0xa9:
	case 0xaa:
	case 0xab:
	case 0xb1:
		process_epg_summary(buffer, section_length - 4);
		break;
	case 0xa5:
	case 0xa6:
	case 0xa7:
	/* Unknown but a5, a6, a7 are the same */
		process_epg_test_a5_a6_a7(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	case 0xb5:
	/* Firmware */
		//process_epg_test_b5(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	case 0xb6:
	/* Firmware */
		//process_epg_test_b6(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	case 0xc0:
	/* Unknown */
		process_epg_test_c0(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	case 0xc1:
	/* Unknown */
		process_epg_test_c1(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	case 0xc2:
	/* Unknown */
		process_epg_test_c2(buffer, section_length - 4);
		printf("demux_ts: Mystery EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	default:
		printf("demux_ts: Unknown EPG type 0x%x, PID=0x%x\n", buffer[0], pid);
		break;
	}
#if 0
	/* Check CRC. */
	for (n = 0; n < section_length + 2; n++) {
		calc_crc32 = demux_ts_compute_crc32(this,
			buffer,
			n, 0xffffffff);
		printf("%04x:%04x\n", n, calc_crc32);
	}
#endif

}

static void process_sdt(struct demux_ts_s *this, int pid)
{
	struct program_s *program;
	int section_length;
	uint32_t crc32;
	uint32_t calc_crc32;
	uint8_t *buffer;
	int n;
	uint32_t	reserved1;
	uint32_t	pcr_pid;
	uint32_t	reserved2;
	uint32_t       program_info_length;
	uint32_t	stream_type;
	uint32_t	elementary_pid;
	uint32_t	es_info_length;
	uint32_t	desc_tag;
	uint32_t	desc_len;
	uint32_t	ca_system_id;
	uint32_t	ca_pid;
	uint32_t	offset;
	int		program_count;
	int		i;
	int		m;

#ifdef TS_PMT_LOG
  printf ("ts_demux: have all TS packets for the SDT section\n");
#endif
	program_count = this->pids[pid].program_count;
	program = &(this->programs[program_count]);
	buffer = this->pids[pid].section.buffer;
	section_length = this->pids[pid].section.size;
	printf("buffer=%p\n", buffer);
	i = 0;
	for(n = 0; n < section_length + 3; n++) {
		printf("%02x ", buffer[n]);
		
		if ((n % 32) == 31) {
			for (m = i; m < (i + 32); m++) {
				if (buffer[m] > 31 && buffer[m] < 127) {
					printf("%c", buffer[m]);
				} else {
					printf(".", buffer[m]);
				}
			}
			i = n + 1;
			printf("\n");
		}
	}
	for (m = i; m < (n); m++) {
		if (buffer[m] > 31 && buffer[m] < 127) {
			printf("%c", buffer[m]);
		} else {
			printf(".", buffer[m]);
		}
	}
	printf("\n");

	crc32  = (uint32_t) buffer[section_length-4] << 24;
	crc32 |= (uint32_t) buffer[section_length-3] << 16;
	crc32 |= (uint32_t) buffer[section_length-2] << 8;
	crc32 |= (uint32_t) buffer[section_length-1] ;

	/* Check CRC. */
	calc_crc32 = demux_ts_compute_crc32(this,
		buffer,
		section_length - 4, 0xffffffff);
	if (crc32 != calc_crc32) {
		printf ("demux_ts: demux error! SDT with invalid CRC32: packet_crc32: %#.8x calc_crc32: %#.8x\n",
			crc32,calc_crc32);
		return;
	} else {
#ifdef TS_PMT_LOG
		printf ("demux_ts: SDT CRC32 ok: %#.8x\n", crc32);
#endif
	}
	switch (buffer[0]) {
	case 0x42:
		/* Contains names of channels on this stream */
		parse_sdt_actual(this, pid);
		break;
	case 0x46:
		parse_sdt_actual(this, pid);
		//process_epg_suppliment_channels(buffer, section_length - 4);
		break;
	case 0x4a:
		process_epg_channels(buffer, section_length - 4);
		break;
/* Also present on PID 0x12, Table id 0x4e. Probably EIT now and next */
	default:
		printf("demux_ts: Unknown SDT type 0x%x PID=0x%x\n", buffer[0], pid);
	}
}

/*
 * NAME demux_ts_parse_pmt
 *
 * Parse a PMT. The PMT is expected to be exactly one section long,
 * and that section is expected to be contained in a single TS packet.
 *
 * In other words, the PMT is assumed to describe a reasonable number of
 * video, audio and other streams (with descriptors).
 * FIXME: Implement support for multi section PMT.
 */

static void process_pmt(struct demux_ts_s *this, int pid)
{
	struct program_s *program;
	int section_length;
	uint32_t crc32;
	uint32_t calc_crc32;
	uint8_t *buffer;
	int n,m;
	uint32_t	reserved1;
	uint32_t	pcr_pid;
	uint32_t	reserved2;
	uint32_t       program_info_length;
	uint32_t	stream_type;
	uint32_t	elementary_pid;
	uint32_t	es_info_length;
	uint32_t	desc_tag;
	uint32_t	desc_len;
	uint32_t	ca_system_id;
	uint32_t	ca_pid;
	uint32_t	offset;
	int		program_count;

#ifdef TS_PMT_LOG
  printf ("ts_demux: have all TS packets for the PMT section\n");
#endif
	program_count = this->pids[pid].program_count;
	program = &(this->programs[program_count]);
	buffer = this->pids[pid].section.buffer;
	section_length = this->pids[pid].section.size;

	for(n = 0; n < section_length + 3; n++) {
		printf("%02x ", buffer[n]);
		if ((n % 32) == 31) {
		printf("\n");
		}
	}
	printf("\n");

	crc32  = (uint32_t) buffer[section_length+3-4] << 24;
	crc32 |= (uint32_t) buffer[section_length+3-3] << 16;
	crc32 |= (uint32_t) buffer[section_length+3-2] << 8;
	crc32 |= (uint32_t) buffer[section_length+3-1] ;

	/* Check CRC. */
	calc_crc32 = demux_ts_compute_crc32(this,
		buffer,
		section_length + 3 - 4, 0xffffffff);
	if (crc32 != calc_crc32) {
		printf ("demux_ts: demux error! PMT with invalid CRC32: packet_crc32: %#.8x calc_crc32: %#.8x\n",
			crc32,calc_crc32);
		return;
	} else {
#ifdef TS_PMT_LOG
		printf ("demux_ts: PMT CRC32 ok: %#.8x\n", crc32);
#endif
	}
	pcr_pid                   = (((uint32_t) buffer[8] << 8) | buffer[9]) & 0x1fff;
	program_info_length       = (((uint32_t) buffer[10] << 8) | buffer[11]) & 0x0fff;
	printf ("              pcr_pid: 0x%04x\n", pcr_pid);
	printf ("              program_info_length: 0x%04x\n", program_info_length);
	/* Program info descriptor is currently just ignored. */
	printf ("demux_ts: program_info_desc: ");
	for (n = 0; n < program_info_length; n++)
		printf ("%.2x ", buffer[12+n]);
	printf ("\n");
	offset = 12 + program_info_length;
	for (offset = 12 + program_info_length; offset < section_length - 1; ) {
		printf("offset = %d, section_length = %d\n", offset, section_length);
		stream_type = buffer[offset];
		elementary_pid = (((uint32_t) buffer[offset + 1] << 8) | buffer[offset + 2]) & 0x1fff;
		es_info_length       = (((uint32_t) buffer[offset + 3] << 8) | buffer[offset + 4]) & 0x0fff;
		if (stream_type == 2) {
			program->video.pid = elementary_pid;
			this->pids[elementary_pid].type = PID_TYPE_VIDEO_MPEG2;
			this->pids[elementary_pid].program_count = program_count;
		} else if (stream_type == 4) {
			program->audio.pid = elementary_pid;
			this->pids[elementary_pid].type = PID_TYPE_AUDIO_MPEG;
			this->pids[elementary_pid].program_count = program_count;
		} else if (stream_type == 5) {
			this->pids[elementary_pid].type = PID_TYPE_PRIV_SECT;
			this->pids[elementary_pid].program_count = program_count;
		} else if (stream_type == 6) {
			this->pids[elementary_pid].type = PID_TYPE_PRIV_PES;
			this->pids[elementary_pid].program_count = program_count;
		} else if (stream_type == 0x1b) {
			program->video.pid = elementary_pid;
			this->pids[elementary_pid].type = PID_TYPE_VIDEO_H264;
			this->pids[elementary_pid].program_count = program_count;
		} else if (stream_type == 0x81) {
			program->audio.pid = elementary_pid;
			this->pids[elementary_pid].type = PID_TYPE_AUDIO_AC3;
			this->pids[elementary_pid].program_count = program_count;
		} else {
			this->pids[elementary_pid].type = PID_TYPE_UNKNOWN;
			this->pids[elementary_pid].program_count = program_count;
		}
		printf ("              stream_type: 0x%02x\n", stream_type);
		printf ("              elementary_pid: 0x%04x\n", elementary_pid);
		printf ("              es_info_length: 0x%04x\n", es_info_length);
		for (n = 0; n < es_info_length; ) {
			desc_tag = buffer[offset + 5 + n];
			desc_len = buffer[offset + 5 + n + 1];
			ca_system_id = 0;
			ca_pid = 0;
			/* Handle CA */
			if (desc_tag == 9) {
				ca_system_id = (((uint32_t) buffer[offset + 5 + n + 2] << 8) |
						 buffer[offset + 5 + n + 3]);
				ca_pid = (((uint32_t) buffer[offset + 5 + n + 4] << 8) |
						 buffer[offset + 5 + n + 5]) & 0x1fff;
				this->pids[ca_pid].type = PID_TYPE_CA_ECM;
				this->pids[ca_pid].program_count = program_count;
				this->pids[elementary_pid].pid_for_ecm = ca_pid;
				if (stream_type == 2) {
					program->video.ca_pid = ca_pid;
				}
				if (stream_type == 4) {
					program->audio.ca_pid = ca_pid;
				}
			}
			printf ("              es_tag: 0x%02x\n", desc_tag);
			printf ("              es_len: 0x%02x  ", desc_len);
			for(m = 0; m < desc_len; m++) {
				printf("%02x ", buffer[offset + 5 + n + 2 + m]);
			};
			for(m = 0; m < desc_len; m++) {
				int tmp;
				tmp = buffer[offset + 5 + n + 2 + m];
				if ((tmp > 32) && (tmp < 127))
					printf("%c", tmp);
				else
					printf(".", tmp);
			};
			printf("\n");
			if (desc_tag == 9) {
				printf ("              ca_system_id: 0x%04x\n", ca_system_id);
				printf ("              ca_pid: 0x%04x\n", ca_pid);
			}
			n += desc_len + 2;
		}
		offset += 5 + es_info_length;
		printf("\n");
	}
		
}

static int64_t demux_ts_adaptation_field_parse(uint8_t *data,
					       uint32_t adaptation_field_length) {

  uint32_t    discontinuity_indicator=0;
  uint32_t    random_access_indicator=0;
  uint32_t    elementary_stream_priority_indicator=0;
  uint32_t    PCR_flag=0;
  int64_t     PCR=0;
  uint32_t    EPCR=0;
  uint32_t    OPCR_flag=0;
  uint32_t    OPCR=0;
  uint32_t    EOPCR=0;
  uint32_t    slicing_point_flag=0;
  uint32_t    transport_private_data_flag=0;
  uint32_t    adaptation_field_extension_flag=0;
  uint32_t    offset = 1;

  discontinuity_indicator = ((data[0] >> 7) & 0x01);
  random_access_indicator = ((data[0] >> 6) & 0x01);
  elementary_stream_priority_indicator = ((data[0] >> 5) & 0x01);
  PCR_flag = ((data[0] >> 4) & 0x01);
  OPCR_flag = ((data[0] >> 3) & 0x01);
  slicing_point_flag = ((data[0] >> 2) & 0x01);
  transport_private_data_flag = ((data[0] >> 1) & 0x01);
  adaptation_field_extension_flag = (data[0] & 0x01);

#ifdef TS_LOG
  printf ("demux_ts: ADAPTATION FIELD length: %d (%x)\n",
          adaptation_field_length, adaptation_field_length);
  if(discontinuity_indicator) {
    printf ("               Discontinuity indicator: %d\n",
            discontinuity_indicator);
  }
  if(random_access_indicator) {
    printf ("               Random_access indicator: %d\n",
            random_access_indicator);
  }
  if(elementary_stream_priority_indicator) {
    printf ("               Elementary_stream_priority_indicator: %d\n",
            elementary_stream_priority_indicator);
  }
#endif
  if(PCR_flag) {
    PCR  = (((int64_t) data[offset]) & 0xFF) << 25;
    PCR += (int64_t) ((data[offset+1] & 0xFF) << 17);
    PCR += (int64_t) ((data[offset+2] & 0xFF) << 9);
    PCR += (int64_t) ((data[offset+3] & 0xFF) << 1);
    PCR += (int64_t) ((data[offset+4] & 0x80) >> 7);

    EPCR = ((data[offset+4] & 0x1) << 8) | data[offset+5];
#ifdef TS_LOG
    printf ("demux_ts: PCR: %"PRId64", EPCR: %u\n",
            PCR, EPCR);
#endif
    offset+=6;
  }
  if(OPCR_flag) {
    OPCR = data[offset] << 25;
    OPCR |= data[offset+1] << 17;
    OPCR |= data[offset+2] << 9;
    OPCR |= data[offset+3] << 1;
    OPCR |= (data[offset+4] >> 7) & 0x01;
    EOPCR = ((data[offset+4] & 0x1) << 8) | data[offset+5];
#ifdef TS_LOG
    printf ("demux_ts: OPCR: %u, EOPCR: %u\n",
            OPCR,EOPCR);
#endif
    offset+=6;
  }
#ifdef TS_LOG
  if(slicing_point_flag) {
    printf ("demux_ts: slicing_point_flag: %d\n",
            slicing_point_flag);
  }
  if(transport_private_data_flag) {
    printf ("demux_ts: transport_private_data_flag: %d\n",
	    transport_private_data_flag);
  }
  if(adaptation_field_extension_flag) {
    printf ("demux_ts: adaptation_field_extension_flag: %d\n",
            adaptation_field_extension_flag);
  }
#endif
  return PCR;
}

/* check if an apid is in the list of known apids */

/* transport stream packet layer */
static void demux_ts_parse_packet (struct demux_ts_s *this, uint8_t *packet)
{

	uint8_t *originalPkt;
	uint8_t   sync_byte;
	uint8_t   transport_error_indicator;
	uint8_t   payload_unit_start_indicator;
	uint8_t   transport_priority;
	uint16_t   pid;
	uint8_t   transport_scrambling_control;
	uint8_t   adaptation_field_control;
	uint8_t   continuity_counter;
	uint8_t   expected_last_continuity_counter;
	uint32_t   data_offset;
	uint32_t   data_len;
	int pes_stream_id;
	int	discontinuity = 0;
	uint32_t       program_count;
	int i;
	int n;
	static int ccc=0;

#if 0
	/* get next synchronised packet, or NULL */
	originalPkt = demux_synchronise(this);
	if (originalPkt == NULL)
		return;
#endif
	originalPkt = packet;

	sync_byte                      = originalPkt[0];
	transport_error_indicator      = (originalPkt[1]  >> 7) & 0x01;
	payload_unit_start_indicator   = (originalPkt[1] >> 6) & 0x01;
	transport_priority             = (originalPkt[1] >> 5) & 0x01;
	pid                            = ((originalPkt[1] << 8) |
				    originalPkt[2]) & 0x1fff;
	transport_scrambling_control   = (originalPkt[3] >> 6)  & 0x03;
	adaptation_field_control       = (originalPkt[3] >> 4) & 0x03;
	continuity_counter             = originalPkt[3] & 0x0f;
	expected_last_continuity_counter = (continuity_counter - 1) & 0x0f;
	if (expected_last_continuity_counter != this->pids[pid].last_continuity_counter) {
		discontinuity = 1;
	}
	this->pids[pid].last_continuity_counter = continuity_counter;	
	program_count = this->pids[pid].program_count;

#ifdef TS_HEADER_LOG
	printf("demux_ts:ts_header:sync_byte=0x%.2x\n",sync_byte);
	printf("demux_ts:ts_header:transport_error_indicator=%d\n", transport_error_indicator);
	printf("demux_ts:ts_header:payload_unit_start_indicator=%d\n", payload_unit_start_indicator);
	printf("demux_ts:ts_header:transport_priority=%d\n", transport_priority);
	printf("demux_ts:ts_header:pid=0x%.4x\n", pid);
	printf("demux_ts:ts_header:transport_scrambling_control=0x%.1x\n", transport_scrambling_control);
	printf("demux_ts:ts_header:adaptation_field_control=0x%.1x\n", adaptation_field_control);
	printf("demux_ts:ts_header:continuity_counter=0x%.1x\n", continuity_counter);

	for(n = 0; n < 188; n++) {
		printf("%02x ", packet[n]);
		if ((n % 32) == 31)
			printf("\n");
	}
	printf("\n");
#endif
	/*
	 * Discard packets that are obviously bad.
	 */
	if (sync_byte != SYNC_BYTE) {
		printf ( "demux error! invalid ts sync byte %.2x\n", sync_byte);
		return;
	}
	if (transport_error_indicator) {
		printf ("demux error! transport error\n");
		return;
	}
	if (pid == 0x1ffb) {
		/* printf ("demux_ts: PSIP table. Program Guide etc....not supported yet. PID = 0x1ffb\n"); */
		return;
	}

	if (transport_scrambling_control) {
#ifdef TS_SCRAM
		printf ("demux_ts: PID 0x%.4x is scrambled!, sc=%d\n", pid, transport_scrambling_control);
#endif
		return;
	} else {
#ifdef TS_SCRAM
		printf ("demux_ts: PID 0x%.4x is not scrambled!, sc=%d\n", pid, transport_scrambling_control);
#endif
	}


	data_offset = 4;

	if( adaptation_field_control & 0x2 ){
		uint32_t adaptation_field_length = originalPkt[4];
		if (adaptation_field_length > 0) {
			demux_ts_adaptation_field_parse (originalPkt+5, adaptation_field_length);
		}
		/*
		 * Skip adaptation header.
		 */
		data_offset += adaptation_field_length + 1;
	}

	if (! (adaptation_field_control & 0x1)) {
		return;
	}

	data_len = PKT_SIZE - data_offset;
#ifdef TS_HEADER_LOG
	printf("data_offset:0x%x data_len:0x%x\n", data_offset, data_len);
#endif

	if (pid == 0) {
		demux_ts_parse_pat (this, originalPkt, data_offset-4,
			payload_unit_start_indicator);
		return;
	}
	if (pid == 1) {
		demux_ts_parse_cat (this, originalPkt, data_offset-4,
			payload_unit_start_indicator);
		return;
	}
	if (pid == 0x11 || pid == 0x12 ) {
#ifdef TS_HEADER_LOG
		printf ("demux_ts: SDT pid: 0x%.4x\n",
			pid);
#endif
		//printf("sdt find1: 0x%x, service_count=0x%x, %s, %s\n", ccc, 0xe, this->services[0xe].provider, this->services[0xe].name);
		demux_ts_parse_section_si(this, originalPkt, data_offset-4,
			payload_unit_start_indicator,
			&this->pids[pid].section,
			discontinuity, pid);
		//printf("sdt find2: 0x%x, service_count=0x%x, %s, %s\n", ccc, 0xe, this->services[0xe].provider, this->services[0xe].name);
		ccc++;
		/* Do we have a complete SDT now */
		if (this->pids[pid].section.size) {
			process_sdt(this, pid);
			this->pids[pid].section.size = 0;
			this->pids[pid].section.buffer_progress = 0;
#ifdef TS_HEADER_LOG
			printf("Zeroing section.size for pid 0x%x\n", pid);
#endif
		}
		return;
	}
#if 1
	if (pid >= 0x30 && pid < 0x62) {
#ifdef TS_HEADER_LOG
		printf ("demux_ts: EPG pid: 0x%.4x\n",
			pid);
#endif
		//printf("sdt find3: 0x%x, service_count=0x%x, %s, %s\n", ccc, 0xe, this->services[0xe].provider, this->services[0xe].name);
		demux_ts_parse_section_si(this, originalPkt, data_offset-4,
			payload_unit_start_indicator,
			&this->pids[pid].section,
			discontinuity, pid);
		//printf("sdt find4: 0x%x, service_count=0x%x, %s, %s\n", ccc, 0xe, this->services[0xe].provider, this->services[0xe].name);
		ccc++;
		/* Do we have a complete EPG now */
		if (this->pids[pid].section.size) {
			process_epg(this, pid);
			this->pids[pid].section.size = 0;
		}
		return;
	}
#endif
	/*
	 * audio/video pid auto-detection, if necessary
	 */
#if 0
	if (this->pids[pid].type == PID_TYPE_PMT) {
		
#ifdef TS_PMT_LOG
		printf ("demux_ts: PMT prog: 0x%.4x pid: 0x%.4x\n",
			this->programs[this->pids[pid].program_count].program_id,
			pid);
#endif
		/* Exclude program number 0, it is a NIT and not a PMT. */
		demux_ts_parse_section(this, originalPkt, data_offset-4,
			payload_unit_start_indicator,
			&this->pids[pid].section,
			discontinuity);
		/* Do we have a complete PMT now */
		if (this->pids[pid].section.size &&
			this->pids[pid].section.progress >= 
			this->pids[pid].section.size) {
			this->pids[pid].section.progress = this->pids[pid].section.size;
			process_pmt(this, pid);
		}
		return;
	}
	if (this->pids[pid].type == PID_TYPE_CA_ECM) {
#ifdef TS_LOG
		printf ("demux_ts: CA prog: 0x%.4x pid: 0x%.4x\n",
			this->programs[this->pids[pid].program_count].program_id,
			pid);
		demux_ts_parse_ecm (this, originalPkt, data_offset-4,
			payload_unit_start_indicator);
#endif
	}
#endif
	return;
}

#if 0
static int detect_ts(uint8_t *buf, size_t len, int ts_size)
{
  int    i, j;
  int    try_again, ts_detected = 0;
  size_t packs = len / ts_size - 2;

  for (i = 0; i < ts_size; i++) {
    try_again = 0;
    if (buf[i] == SYNC_BYTE) {
      for (j = 1; j < packs; j++) {
	if (buf[i + j*ts_size] != SYNC_BYTE) {
	  try_again = 1;
	  break;
	}
      }
      if (try_again == 0) {
#ifdef TS_LOG
	printf ("demux_ts: found 0x47 pattern at offset %d\n", i);
#endif
	ts_detected = 1;
      }
    }
  }

  return ts_detected;
}
#endif

int main(int argc, char *argv[])
{
	char *filename;
	//char *out_file = "ecm-out.ts";
	int tmp;
	int in_fd;
	int out_fd;
	int l, n, m;
	int pid;
	int scrambling_control;
	int pid_counter = 0;
	int found;
	char *name;
//	struct sNode *H;
//	H = malloc(sizeof(struct sNode));
//        tmp = read_huff_dict( &H );
        tmp = read_huff_dict();
	printf ("read_huff_dict:result = %d\n",tmp);

        if(argc<2) {
                printf("usage: %s <filename.ts>\n",argv[0]);
                return 1;
        }
	filename = argv[1];
	demux_ts.pids = calloc(0x2000, sizeof(struct pid_s));
	for(n = 0; n < 0x2000; n++) {
		demux_ts.pids[n].program_count = INVALID_PROGRAM;
		demux_ts.pids[n].section.buffer = 0;
		demux_ts.pids[n].section.size = 0;
		demux_ts.pids[n].section.buffer_target = 0;
	}
	demux_ts.pids[0].type = PID_TYPE_PAT;

	demux_ts.programs = calloc(256, sizeof(struct program_s));
	for(n = 0; n < 256; n++) {
		section_c0[n].total_length = 0;
	}
	for(n = 0; n < 256; n++) {
		demux_ts.programs[n].program_id = INVALID_PROGRAM;
	}
	demux_ts.services = calloc(MAX_SERVICES, sizeof(struct service_s));
	for(n = 0; n < MAX_SERVICES; n++) {
		demux_ts.services[n].program_id = INVALID_PROGRAM;
		demux_ts.services[n].provider = 0;
		demux_ts.services[n].name = 0;
	}
	demux_ts_build_crc32_table(&demux_ts);

#if 0
    	lThemes = (struct theme_s *) calloc(MAX_THEMES, sizeof(struct theme_s));
    	if (!lThemes) {
    	    printf("failed to allocate memory for lThemes");
    	    return 0;
    	}
#endif
	nChannels = 0;
	lChannels = (struct channel_s *) calloc(MAX_CHANNELS, sizeof(struct channel_s));
    	if (!lChannels) {
            printf("failed to allocate memory for lChannels");
    	    return 0;
    	}
	channels_all = (uint16_t *) calloc(65536, sizeof(uint16_t));
	for(n = 0; n < 65536; n++) {
		channels_all[n] = 0xffff;
	}
	
    	nBouquets = 0;
	lBouquets = (struct bouquet_s *) calloc(MAX_BOUQUETS, sizeof(struct bouquet_s));
    	if (!lBouquets) {
    	    printf("failed to allocate memory for lBouquets");
    	    return 0;
    	}
#if 0
    	lTitles = (struct title_s *) calloc(MAX_TITLES, sizeof(struct title_s));
    	if (!lTitles) {
    	    printf("failed to allocate memory for lTitles");
    	    return 0;
    	}
    	lSummaries = (struct summary_s *) calloc(MAX_SUMMARIES, sizeof(struct summary_s));
    	if (!lSummaries) {
    	    printf("failed to allocate memory for lSummaries");
    	    return 0;
    	}
    	bChannels = (uint8_t *) calloc(MAX_BUFFER_SIZE_CHANNELS, sizeof(uint8_t));
    	if (!bChannels) {
    	    log_message(ERROR, "failed to allocate memory for bChannels");
    	    goto endrunning;
    	}
    	bTitles = (uint8_t *) calloc(MAX_BUFFER_SIZE_TITLES, sizeof(uint8_t));
    	if (!bTitles) {
    	    log_message(ERROR, "failed to allocate memory for bTitles");
    	    goto endrunning;
    	}
    	bSummaries = (uint8_t *) calloc(MAX_BUFFER_SIZE_SUMMARIES, sizeof(uint8_t));
    	if (!bSummaries) {
    	    log_message(ERROR, "failed to allocate memory for bSummaries");
    	    goto endrunning;
    	}
#endif


	tmp = in_fd = open(filename, O_RDONLY | O_NONBLOCK);
	if (tmp < 0) {
		printf("Open failed: %s\n", strerror(errno));
		return 1;
	}
	for(n = 0; ; n++) {
		printf("\n\n");
		tmp = read(in_fd, buffer, 188);
		if (tmp < 188) {
			printf("Read failed: %s\n", strerror(errno));
			break;
		}
		if (buffer[0] != 0x47) {
			printf("Found no sync\n");
			return 1;
		}
		pid = (buffer[2] + (buffer[1] << 8)) & 0x1fff;
		if (pid == 0) {
			memcpy(pat, buffer, 188);
		}
		scrambling_control = (buffer[3] >> 6);
		demux_ts.pids[pid].present = 1;
		if (scrambling_control & 2) {
			demux_ts.pids[pid].scrambling_control = scrambling_control;
		}
		demux_ts_parse_packet(&demux_ts, buffer);
	}
	close(in_fd);

#if 0
	tmp = out_fd = open(out_file, O_CREAT | O_WRONLY | O_NONBLOCK, S_IRWXU);
	if (tmp < 0) {
		printf("Open failed: %s\n", strerror(errno));
		return 1;
	}
	while (1) {
		tmp = read(in_fd, buffer, 188);
		pid_counter++;
		if (tmp < 188) {
			printf("Read failed: %s\n", strerror(errno));
			break;
		}
		if (buffer[0] != 0x47) {
			printf("Found no sync\n");
			return 1;
		}
		pid = (buffer[2] + (buffer[1] << 8)) & 0x1fff;
		printf("maybe write pid=0x%x\n", pid);
	    	if (demux_ts.pids[pid].type == PID_TYPE_CA_ECM) {
			if (!demux_ts.pids[pid].previous_ecm) {
				demux_ts.pids[pid].previous_ecm = calloc(188, 1);
			}
			tmp = ecm_compare(demux_ts.pids[pid].previous_ecm, buffer);
			if (tmp) {
				memcpy(demux_ts.pids[pid].previous_ecm, buffer, 188);
				printf("WRITING PID:%x:%d\n", pid, pid_counter);
				tmp = write(out_fd, buffer, 188);
				printf("Written %d\n", tmp);
			}
		}
	}
	close(out_fd);
#endif		

	for(n = 0; n < 0x2000; n++) {
		if (demux_ts.pids[n].present) {
			printf("PID=0x%04x SC=%d Program=0x%0x type=%d:%s pid_for_ecm=0x%x\n", n, demux_ts.pids[n].scrambling_control, demux_ts.pids[n].program_count, demux_ts.pids[n].type, type[demux_ts.pids[n].type], demux_ts.pids[n].pid_for_ecm);
		}
	}
	for(n = 0; n < 255; n++) {
	    	if (demux_ts.programs[n].program_id != INVALID_PROGRAM) {
			for(m = 0; m < MAX_SERVICES; m++) {
				if (demux_ts.programs[n].program_id == demux_ts.services[m].program_id) {
					demux_ts.programs[n].service_count = m;
					break;
				}
			}
		}
	}

#if 0
				
	for(n = 0; n < 255; n++) {
	    	if (demux_ts.programs[n].program_id != INVALID_PROGRAM) {
			printf ("demux_ts: PAT acquired count=%d programNumber=0x%04x "
				"pmtPid=0x%04x video_pid=0x%04x video_ca_pid=0x%04x "
				"audio_pid=0x%04x audio_ca_pid=0x%04x, name=%s\n",
				n,
				demux_ts.programs[n].program_id,
				demux_ts.programs[n].pid_pmt,
				demux_ts.programs[n].video.pid,
				demux_ts.programs[n].video.ca_pid,
				demux_ts.programs[n].audio.pid,
				demux_ts.programs[n].audio.ca_pid,
				demux_ts.services[demux_ts.programs[n].service_count].name);
		}
	}
	for(n = 0; n < MAX_SERVICES; n++) {
	    	if (demux_ts.services[n].program_id != INVALID_PROGRAM) {
			printf ("demux_ts: SDT acquired count=%d programNumber(Sid)=0x%04x "
				"provider=%s name=%s\n",
				n,
				demux_ts.services[n].program_id,
				demux_ts.services[n].provider,
				demux_ts.services[n].name);
		}
	}
	printf("nChannels = 0x%x\n", nChannels);
	for(n = 0; n < nChannels; n++) {
		qsort( lChannels[n].events, lChannels[n].events_count, sizeof( struct event_s ), &qsort_events_by_time );
	    	if (lChannels[n].ChannelId == 0x540) {
//	    	if (lChannels[n].ChannelId == 0x86a) {
			found = MAX_SERVICES;
			for(m = 0; m < MAX_SERVICES; m++) {
				if (demux_ts.services[m].program_id == lChannels[n].Sid) {
					found = m;
					break;
				}
			}
			if (found == MAX_SERVICES) {
				name="JCD:NOT FOUND";
			} else {
				name=demux_ts.services[found].name;
			}
			printf ("demux_ts: CHANNELS 0x%04x:ChannelID = 0x%x, Nid = 0x%x, Tid = 0x%x, Sid = 0x%x, SkyNumber = %d, %d, Info = 0x%x:%s, events_count=%d, Name=%s\n",
				n,
		      		lChannels[n].ChannelId,
				lChannels[n].Nid,
				lChannels[n].Tid,
				lChannels[n].Sid,
				lChannels[n].SkyNumber1,
				lChannels[n].SkyNumber2,
				lChannels[n].info,
				info_to_string(lChannels[n].info),
				lChannels[n].events_count,
				name);
			if (lChannels[n].events_count) {
				for(l = 0; l < lChannels[n].events_count; l++) {
					struct tm tm1, *tm2;
					int fail = 0;
					uint64_t next_time;
					next_time = lChannels[n].events[l].start_time_title + lChannels[n].events[l].duration_title;
					if ((lChannels[n].events[l].start_time_title + lChannels[n].events[l].duration_title) != (lChannels[n].events[l + 1].start_time_title) ) {
						fail = 1;
					}
					tm2 = &tm1;
					tm2 = gmtime_r(&lChannels[n].events[l].start_time_title, &tm1);
					printf("demux_ts: EVENT %04x: %04d-%02d-%02d %02d:%02d:%02d, %ld, %ld, %ld, %ld, %d, %s,,,%s\n",
						lChannels[n].events[l].event_id,
						tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday,
						tm1.tm_hour, tm1.tm_min, tm1.tm_sec,
						lChannels[n].events[l].start_time_title,
						lChannels[n].events[l].duration_title,
						next_time,
						lChannels[n].events[l + 1].start_time_title,
						fail,
						lChannels[n].events[l].title,
						lChannels[n].events[l].summary);
				}
			} 
//		      C->pData = 0;
//		      C->lenData = 0;
//		      C->IsFound = 0;
//		      C->IsEpg = 1;
		}
	}
#endif

	return 0;
}



