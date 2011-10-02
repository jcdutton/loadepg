/* dump -- tool to dump stats regarding a dvb stream from a file
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

#if 1
#define TS_LOG 1
#define TS_SCRAM 1
#define TS_PAT_LOG 1
#define TS_PMT_LOG 1
#define TS_HEADER_LOG 1
#endif
#define TS_PAT_LOG 1
#define TS_PMT_LOG 1
#define TS_SCRAM 1

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

uint8_t buffer[200];

struct section_s {
	int size;
	int progress;
	uint8_t *buffer;
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

static void demux_ts_parse_section (struct demux_ts_s *this,
	uint8_t *original_pkt,
	uint32_t	offset,
	unsigned int   pusi,
	struct section_s *section,
	int		discontinuity)
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
	unsigned int	 pid;
	unsigned char *stream;
	unsigned int	 i;
	int		 count;
	char		*ptr = NULL;
	unsigned char  len;
	uint32_t	tmp32;
	uint32_t	offset_section_start;
	uint8_t		*pkt;
	uint32_t	progress;

	/*
	 * A new section should start with the payload unit start
	 * indicator set. We allocate some mem (max. allowed for a PM section)
	 * to copy the complete section into one chunk.
	 */
	if (pusi) {
		/* pointer to start of section. */
		/* Only exists if pusi is set. */
		tmp32 = original_pkt[offset + 4];
		offset_section_start = offset + tmp32 + 5;
		pkt = original_pkt + offset_section_start;

		table_id                  =  pkt[0] ;
		section_syntax_indicator  = (pkt[1] >> 7) & 0x01;
		section_length            = (((uint32_t) pkt[1] << 8) | pkt[2]) & 0x03ff;
		program_number            =  ((uint32_t) pkt[3] << 8) | pkt[4];
		version_number            = (pkt[5] >> 1) & 0x1f;
		current_next_indicator    =  pkt[5] & 0x01;
		section_number            =  pkt[6];
		last_section_number       =  pkt[7];
		pcr_pid                   = (((uint32_t) pkt[8] << 8) | pkt[9]) & 0x1fff;
		program_info_length       = (((uint32_t) pkt[10] << 8) | pkt[11]) & 0x0fff;
		
		/* Exclude program number 0, it is a NIT and not a PMT. */
		section->size = section_length;
		section->progress = 188-offset_section_start;
		if (!section->buffer) {
			section->buffer = calloc(1024, 1);
			if (!section->buffer) {
				printf("OUT OF MEMORY!!!!\n");
				return;
			}
		}
		memcpy (section->buffer, pkt, 188 - offset_section_start);

#ifdef TS_PMT_LOG
		printf ("demux_ts: SECTION table_id: %2x\n", table_id);
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
		printf ("              progress: %d\n", section->progress);
#endif

		if ((section_syntax_indicator != 1) || !current_next_indicator) {
#ifdef TS_PMT_LOG
			printf ("ts_demux: section_syntax_indicator != 1 "
				"|| !current_next_indicator\n");
#endif
			return;
		}
	} else {
		printf ("demux_ts: section !pusi\n");
		if (discontinuity) {
			section->size = 0;
			printf ("demux_ts: section !pusi discontinuity\n");
			return;
		}
		/* Wait for pusi */
		if (!(section->size)) {
			return;
		}
		section_length = section->size;
		progress = section->progress;
		len = 188 - offset - 4;
		printf("!pusi: offset = 0x%04x len = 0x%04x\n", offset, len);
		memcpy (section->buffer + progress, original_pkt + offset + 4, len);
		section->progress += len;
	}
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
			printf("Unknown tag\n");
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
		printf("sdt actual: serice_id:0x%x len:0x%x\n", service_id, descriptors_loop_len);
		service_count = 0;

		while ((this->services[service_count].program_id != INVALID_PROGRAM) &&
			(this->services[service_count].program_id != service_id)  &&
			(service_count < 255 ) ) {
			service_count++;
		}
		this->services[service_count].program_id = service_id;

		process_sdt_descriptors( this, &(this->services[service_count]), &buffer[n + 5], descriptors_loop_len );
		n += descriptors_loop_len + 5;
	}
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

	crc32  = (uint32_t) buffer[section_length+3-4] << 24;
	crc32 |= (uint32_t) buffer[section_length+3-3] << 16;
	crc32 |= (uint32_t) buffer[section_length+3-2] << 8;
	crc32 |= (uint32_t) buffer[section_length+3-1] ;

	/* Check CRC. */
	calc_crc32 = demux_ts_compute_crc32(this,
		buffer,
		section_length + 3 - 4, 0xffffffff);
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
	default:
		printf("demux_ts: Unknown SDT type\n");
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
	printf("data_len:0x%x\n", data_len);
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
	if (pid == 0x11 || pid == 0x12) {
		printf ("demux_ts: SDT pid: 0x%.4x\n",
			pid);
		demux_ts_parse_section(this, originalPkt, data_offset-4,
			payload_unit_start_indicator,
			&this->pids[pid].section,
			discontinuity);
		/* Do we have a complete SDT now */
		if (this->pids[pid].section.size &&
			this->pids[pid].section.progress >= 
			this->pids[pid].section.size) {
			this->pids[pid].section.progress = this->pids[pid].section.size;
			process_sdt(this, pid);
		}
		return;
	}
	/*
	 * audio/video pid auto-detection, if necessary
	 */
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
	int n,m;
	int pid;
	int scrambling_control;
	int pid_counter = 0;

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
		demux_ts.pids[n].section.progress = 0;
	}
	demux_ts.pids[0].type = PID_TYPE_PAT;

	demux_ts.programs = calloc(256, sizeof(struct program_s));
	for(n = 0; n < 256; n++) {
		demux_ts.programs[n].program_id = INVALID_PROGRAM;
	}
	demux_ts.services = calloc(256, sizeof(struct service_s));
	for(n = 0; n < 256; n++) {
		demux_ts.services[n].program_id = INVALID_PROGRAM;
		demux_ts.services[n].provider = 0;
		demux_ts.services[n].name = 0;
	}
	demux_ts_build_crc32_table(&demux_ts);

	tmp = in_fd = open(filename, O_RDONLY | O_NONBLOCK);
	if (tmp < 0) {
		printf("Open failed: %s\n", strerror(errno));
		return 1;
	}
	for(n = 0; ; n++) {
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
	tmp = in_fd = open(filename, O_RDONLY | O_NONBLOCK);
	if (tmp < 0) {
		printf("Open failed: %s\n", strerror(errno));
		return 1;
	}
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
			for(m = 0; m < 255; m++) {
				if (demux_ts.programs[n].program_id == demux_ts.services[m].program_id) {
					demux_ts.programs[n].service_count = m;
					break;
				}
			}
		}
	}
				
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
	for(n = 0; n < 255; n++) {
	    	if (demux_ts.services[n].program_id != INVALID_PROGRAM) {
			printf ("demux_ts: SDT acquired count=%d programNumber=0x%04x "
				"provider=%s name=%s\n",
				n,
				demux_ts.services[n].program_id,
				demux_ts.services[n].provider,
				demux_ts.services[n].name);
		}
	}


	return 0;
}



