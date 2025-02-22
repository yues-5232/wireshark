/**-*-C-*-**********************************************************************
 *
 * text2pcap.c
 *
 * Utility to convert an ASCII hexdump into a libpcap-format capture file
 *
 * (c) Copyright 2001 Ashok Narayanan <ashokn@cisco.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 *******************************************************************************/

/*******************************************************************************
 *
 * This utility reads in an ASCII hexdump of this common format:
 *
 * 00000000  00 E0 1E A7 05 6F 00 10 5A A0 B9 12 08 00 46 00 .....o..Z.....F.
 * 00000010  03 68 00 00 00 00 0A 2E EE 33 0F 19 08 7F 0F 19 .h.......3......
 * 00000020  03 80 94 04 00 00 10 01 16 A2 0A 00 03 50 00 0C .............P..
 * 00000030  01 01 0F 19 03 80 11 01 1E 61 00 0C 03 01 0F 19 .........a......
 *
 * Each bytestring line consists of an offset, one or more bytes, and
 * text at the end. An offset is defined as a hex string of more than
 * two characters. A byte is defined as a hex string of exactly two
 * characters. The text at the end is ignored, as is any text before
 * the offset. Bytes read from a bytestring line are added to the
 * current packet only if all the following conditions are satisfied:
 *
 * - No text appears between the offset and the bytes (any bytes appearing after
 *   such text would be ignored)
 *
 * - The offset must be arithmetically correct, i.e. if the offset is 00000020,
 *   then exactly 32 bytes must have been read into this packet before this.
 *   If the offset is wrong, the packet is immediately terminated
 *
 * A packet start is signaled by a zero offset.
 *
 * Lines starting with #TEXT2PCAP are directives. These allow the user
 * to embed instructions into the capture file which allows text2pcap
 * to take some actions (e.g. specifying the encapsulation
 * etc.). Currently no directives are implemented.
 *
 * Lines beginning with # which are not directives are ignored as
 * comments. Currently all non-hexdump text is ignored by text2pcap;
 * in the future, text processing may be added, but lines prefixed
 * with '#' will still be ignored.
 *
 * The output is a libpcap packet containing Ethernet frames by
 * default. This program takes options which allow the user to add
 * dummy Ethernet, IP and UDP, TCP or SCTP headers to the packets in order
 * to allow dumps of L3 or higher protocols to be decoded.
 *
 * Considerable flexibility is built into this code to read hexdumps
 * of slightly different formats. For example, any text prefixing the
 * hexdump line is dropped (including mail forwarding '>'). The offset
 * can be any hex number of four digits or greater.
 *
 * This converter cannot read a single packet greater than
 * WTAP_MAX_PACKET_SIZE_STANDARD.  The snapshot length is automatically
 * set to WTAP_MAX_PACKET_SIZE_STANDARD.
 */

#include <config.h>

/*
 * Just make sure we include the prototype for strptime as well
 * (needed for glibc 2.2) but make sure we do this only if not
 * yet defined.
 */
#ifndef __USE_XOPEN
#  define __USE_XOPEN
#endif
#ifndef _XOPEN_SOURCE
#  ifndef __sun
#    define _XOPEN_SOURCE 600
#  endif
#endif

/*
 * Defining _XOPEN_SOURCE is needed on some platforms, e.g. platforms
 * using glibc, to expand the set of things system header files define.
 *
 * Unfortunately, on other platforms, such as some versions of Solaris
 * (including Solaris 10), it *reduces* that set as well, causing
 * strptime() not to be declared, presumably because the version of the
 * X/Open spec that _XOPEN_SOURCE implies doesn't include strptime() and
 * blah blah blah namespace pollution blah blah blah.
 *
 * So we define __EXTENSIONS__ so that "strptime()" is declared.
 */
#ifndef __EXTENSIONS__
#  define __EXTENSIONS__
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wsutil/file_util.h>
#include <cli_main.h>
#include <ui/version_info.h>
#include <wsutil/inet_addr.h>
#include <wsutil/wslog.h>
#include <wsutil/nstime.h>

#ifdef _WIN32
#include <io.h>     /* for _setmode */
#include <fcntl.h>  /* for O_BINARY */
#endif

#include <time.h>
#include <glib.h>

#include <wsutil/ws_getopt.h>

#include <errno.h>
#include <assert.h>

#ifndef HAVE_STRPTIME
# include "wsutil/strptime.h"
#endif

#include "writecap/pcapio.h"
#include "text2pcap.h"

#include "wiretap/wtap.h"

/*--- Options --------------------------------------------------------------------*/

/* File format */
static gboolean use_pcapng = FALSE;

/* Interface name */
static char *interface_name = NULL;

/* Debug level */
static int debug = 0;
/* Be quiet */
static gboolean quiet = FALSE;

/* Dummy Ethernet header */
static gboolean hdr_ethernet = FALSE;
static guint8 hdr_eth_dest_addr[6] = {0x0a, 0x02, 0x02, 0x02, 0x02, 0x02};
static guint8 hdr_eth_src_addr[6]  = {0x0a, 0x02, 0x02, 0x02, 0x02, 0x01};
static guint32 hdr_ethernet_proto = 0;

/* Dummy IP header */
static gboolean hdr_ip = FALSE;
static gboolean hdr_ipv6 = FALSE;
static long hdr_ip_proto = -1;

/* Destination and source addresses for IP header */
static guint32 hdr_ip_dest_addr = 0;
static guint32 hdr_ip_src_addr = 0;
static ws_in6_addr hdr_ipv6_dest_addr = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
static ws_in6_addr hdr_ipv6_src_addr  = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
static ws_in6_addr NO_IPv6_ADDRESS    = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

/* Dummy UDP header */
static gboolean hdr_udp = FALSE;
static guint32 hdr_dest_port = 0;
static guint32 hdr_src_port  = 0;

/* Dummy TCP header */
static gboolean hdr_tcp = FALSE;

/* TCP sequence numbers when has_direction is true */
static guint32 tcp_in_seq_num = 0;
static guint32 tcp_out_seq_num = 0;

/* Dummy SCTP header */
static gboolean hdr_sctp = FALSE;
static guint32 hdr_sctp_src  = 0;
static guint32 hdr_sctp_dest = 0;
static guint32 hdr_sctp_tag  = 0;

/* Dummy DATA chunk header */
static gboolean hdr_data_chunk = FALSE;
static guint8  hdr_data_chunk_type = 0;
static guint8  hdr_data_chunk_bits = 0;
static guint32 hdr_data_chunk_tsn  = 0;
static guint16 hdr_data_chunk_sid  = 0;
static guint16 hdr_data_chunk_ssn  = 0;
static guint32 hdr_data_chunk_ppid = 0;

/* ASCII text dump identification */
static gboolean identify_ascii = FALSE;

static gboolean has_direction = FALSE;
static guint32 direction = PACK_FLAGS_DIRECTION_UNKNOWN;

/*--- Local data -----------------------------------------------------------------*/

/* This is where we store the packet currently being built */
static guint8  packet_buf[WTAP_MAX_PACKET_SIZE_STANDARD];
static guint32 header_length;
static guint32 ip_offset;
static guint32 curr_offset = 0;
static guint32 max_offset = WTAP_MAX_PACKET_SIZE_STANDARD;
static guint32 packet_start = 0;

static int start_new_packet(gboolean);

/* This buffer contains strings present before the packet offset 0 */
#define PACKET_PREAMBLE_MAX_LEN     2048
static char packet_preamble[PACKET_PREAMBLE_MAX_LEN+1];
static int    packet_preamble_len = 0;

/* Number of packets read and written */
static guint32 num_packets_read    = 0;
static guint32 num_packets_written = 0;
static guint64 bytes_written       = 0;

/* Time code of packet, derived from packet_preamble */
static time_t   ts_sec  = 0;
static guint32  ts_nsec = 0;
static char    *ts_fmt  = NULL;
static int      ts_fmt_iso = 0;
static struct tm timecode_default;

static guint8* pkt_lnstart;

/* Input file */
static char *input_filename;
static FILE       *input_file  = NULL;
/* Output file */
static char *output_filename;
static FILE       *output_file = NULL;

/* Offset base to parse */
static guint32 offset_base = 16;

extern FILE *text2pcap_in;

/* ----- State machine -----------------------------------------------------------*/

/* Current state of parser */
typedef enum {
    INIT,             /* Waiting for start of new packet */
    START_OF_LINE,    /* Starting from beginning of line */
    READ_OFFSET,      /* Just read the offset */
    READ_BYTE,        /* Just read a byte */
    READ_TEXT         /* Just read text - ignore until EOL */
} parser_state_t;
static parser_state_t state = INIT;

static const char *state_str[] = {"Init",
                           "Start-of-line",
                           "Offset",
                           "Byte",
                           "Text"
};

static const char *token_str[] = {"",
                           "Byte",
                           "Offset",
                           "Directive",
                           "Text",
                           "End-of-line"
};

/* ----- Skeleton Packet Headers --------------------------------------------------*/

typedef struct {
    guint8  dest_addr[6];
    guint8  src_addr[6];
    guint16 l3pid;
} hdr_ethernet_t;

static hdr_ethernet_t HDR_ETHERNET;

typedef struct {
    guint8  ver_hdrlen;
    guint8  dscp;
    guint16 packet_length;
    guint16 identification;
    guint8  flags;
    guint8  fragment;
    guint8  ttl;
    guint8  protocol;
    guint16 hdr_checksum;
    guint32 src_addr;
    guint32 dest_addr;
} hdr_ip_t;

/* Fixed IP address values */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define IP_ID  0x1234
#define IP_SRC 0x0a010101
#define IP_DST 0x0a020202
#else
#define IP_ID  0x3412
#define IP_SRC 0x0101010a
#define IP_DST 0x0202020a
#endif

static hdr_ip_t HDR_IP = {0x45, 0, 0, IP_ID, 0, 0, 0xff, 0, 0, IP_SRC, IP_DST};

static struct {         /* pseudo header for checksum calculation */
    guint32 src_addr;
    guint32 dest_addr;
    guint8  zero;
    guint8  protocol;
    guint16 length;
} pseudoh;


/* headers taken from glibc */

typedef struct {
    union  {
        struct ip6_hdrctl {
            guint32 ip6_un1_flow;   /* 24 bits of flow-ID */
            guint16 ip6_un1_plen;   /* payload length */
            guint8  ip6_un1_nxt;    /* next header */
            guint8  ip6_un1_hlim;   /* hop limit */
        } ip6_un1;
        guint8 ip6_un2_vfc;       /* 4 bits version, 4 bits priority */
    } ip6_ctlun;
    ws_in6_addr ip6_src;      /* source address */
    ws_in6_addr ip6_dst;      /* destination address */
} hdr_ipv6_t;

static hdr_ipv6_t HDR_IPv6;

/* https://tools.ietf.org/html/rfc2460#section-8.1 */
static struct {                 /* pseudo header ipv6 for checksum calculation */
    struct  e_in6_addr src_addr6;
    struct  e_in6_addr dst_addr6;
    guint32 length;
    guint8 zero[3];
    guint8 next_header;
} pseudoh6;


typedef struct {
    guint16 source_port;
    guint16 dest_port;
    guint16 length;
    guint16 checksum;
} hdr_udp_t;

static hdr_udp_t HDR_UDP = {0, 0, 0, 0};

typedef struct {
    guint16 source_port;
    guint16 dest_port;
    guint32 seq_num;
    guint32 ack_num;
    guint8  hdr_length;
    guint8  flags;
    guint16 window;
    guint16 checksum;
    guint16 urg;
} hdr_tcp_t;

static hdr_tcp_t HDR_TCP = {0, 0, 0, 0, 0x50, 0, 0, 0, 0};

typedef struct {
    guint16 src_port;
    guint16 dest_port;
    guint32 tag;
    guint32 checksum;
} hdr_sctp_t;

static hdr_sctp_t HDR_SCTP = {0, 0, 0, 0};

typedef struct {
    guint8  type;
    guint8  bits;
    guint16 length;
    guint32 tsn;
    guint16 sid;
    guint16 ssn;
    guint32 ppid;
} hdr_data_chunk_t;

static hdr_data_chunk_t HDR_DATA_CHUNK = {0, 0, 0, 0, 0, 0, 0};

static char tempbuf[64];

/*----------------------------------------------------------------------
 * Stuff for writing a PCap file
 */

/* Link-layer type; see https://www.tcpdump.org/linktypes.html for details */
static guint32 pcap_link_type = 1;   /* Default is LINKTYPE_ETHERNET */

/*----------------------------------------------------------------------
 * Parse a single hex number
 * Will abort the program if it can't parse the number
 * Pass in TRUE if this is an offset, FALSE if not
 */
static int
parse_num(const char *str, int offset, guint32* num)
{
    char    *c;

    if (str == NULL) {
        fprintf(stderr, "FATAL ERROR: str is NULL\n");
        return EXIT_FAILURE;
    }

    *num = (guint32)strtoul(str, &c, offset ? offset_base : 16);
    if (c == str) {
        fprintf(stderr, "FATAL ERROR: Bad hex number? [%s]\n", str);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------
 * Write this byte into current packet
 */
static int
write_byte(const char *str)
{
    guint32 num;

    if (parse_num(str, FALSE, &num) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    packet_buf[curr_offset] = (guint8) num;
    curr_offset++;
    if (curr_offset - header_length >= max_offset) /* packet full */
        if (start_new_packet(TRUE) != EXIT_SUCCESS)
            return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------
 * Write a number of bytes into current packet
 */

static void
write_bytes (const char bytes[], guint32 nbytes)
{
    if (curr_offset + nbytes < WTAP_MAX_PACKET_SIZE_STANDARD) {
        memcpy(&packet_buf[curr_offset], bytes, nbytes);
        curr_offset += nbytes;
    }
}

/*----------------------------------------------------------------------
 * Remove bytes from the current packet
 */
static void
unwrite_bytes (guint32 nbytes)
{
    curr_offset -= nbytes;
}

/*----------------------------------------------------------------------
 * Determine SCTP chunk padding length
 */
static guint32
number_of_padding_bytes (guint32 length)
{
    guint32 remainder;

    remainder = length % 4;

    if (remainder == 0)
        return 0;
    else
        return 4 - remainder;
}

/*----------------------------------------------------------------------
 * Compute one's complement checksum (from RFC1071)
 */
static guint16
in_checksum (void *buf, guint32 count)
{
    guint32 sum = 0;
    guint16 *addr = (guint16 *)buf;

    while (count > 1) {
        /*  This is the inner loop */
        sum += g_ntohs(* (guint16 *) addr);
        addr++;
        count -= 2;
    }

    /*  Add left-over byte, if any */
    if (count > 0)
        sum += g_ntohs(* (guint8 *) addr);

    /*  Fold 32-bit sum to 16 bits */
    while (sum>>16)
        sum = (sum & 0xffff) + (sum >> 16);

    sum = ~sum;
    return g_htons(sum);
}

/* The CRC32C code is taken from draft-ietf-tsvwg-sctpcsum-01.txt.
 * That code is copyrighted by D. Otis and has been modified.
 */

#define CRC32C(c,d) (c=(c>>8)^crc_c[(c^(d))&0xFF])
static guint32 crc_c[256] =
{
0x00000000U, 0xF26B8303U, 0xE13B70F7U, 0x1350F3F4U,
0xC79A971FU, 0x35F1141CU, 0x26A1E7E8U, 0xD4CA64EBU,
0x8AD958CFU, 0x78B2DBCCU, 0x6BE22838U, 0x9989AB3BU,
0x4D43CFD0U, 0xBF284CD3U, 0xAC78BF27U, 0x5E133C24U,
0x105EC76FU, 0xE235446CU, 0xF165B798U, 0x030E349BU,
0xD7C45070U, 0x25AFD373U, 0x36FF2087U, 0xC494A384U,
0x9A879FA0U, 0x68EC1CA3U, 0x7BBCEF57U, 0x89D76C54U,
0x5D1D08BFU, 0xAF768BBCU, 0xBC267848U, 0x4E4DFB4BU,
0x20BD8EDEU, 0xD2D60DDDU, 0xC186FE29U, 0x33ED7D2AU,
0xE72719C1U, 0x154C9AC2U, 0x061C6936U, 0xF477EA35U,
0xAA64D611U, 0x580F5512U, 0x4B5FA6E6U, 0xB93425E5U,
0x6DFE410EU, 0x9F95C20DU, 0x8CC531F9U, 0x7EAEB2FAU,
0x30E349B1U, 0xC288CAB2U, 0xD1D83946U, 0x23B3BA45U,
0xF779DEAEU, 0x05125DADU, 0x1642AE59U, 0xE4292D5AU,
0xBA3A117EU, 0x4851927DU, 0x5B016189U, 0xA96AE28AU,
0x7DA08661U, 0x8FCB0562U, 0x9C9BF696U, 0x6EF07595U,
0x417B1DBCU, 0xB3109EBFU, 0xA0406D4BU, 0x522BEE48U,
0x86E18AA3U, 0x748A09A0U, 0x67DAFA54U, 0x95B17957U,
0xCBA24573U, 0x39C9C670U, 0x2A993584U, 0xD8F2B687U,
0x0C38D26CU, 0xFE53516FU, 0xED03A29BU, 0x1F682198U,
0x5125DAD3U, 0xA34E59D0U, 0xB01EAA24U, 0x42752927U,
0x96BF4DCCU, 0x64D4CECFU, 0x77843D3BU, 0x85EFBE38U,
0xDBFC821CU, 0x2997011FU, 0x3AC7F2EBU, 0xC8AC71E8U,
0x1C661503U, 0xEE0D9600U, 0xFD5D65F4U, 0x0F36E6F7U,
0x61C69362U, 0x93AD1061U, 0x80FDE395U, 0x72966096U,
0xA65C047DU, 0x5437877EU, 0x4767748AU, 0xB50CF789U,
0xEB1FCBADU, 0x197448AEU, 0x0A24BB5AU, 0xF84F3859U,
0x2C855CB2U, 0xDEEEDFB1U, 0xCDBE2C45U, 0x3FD5AF46U,
0x7198540DU, 0x83F3D70EU, 0x90A324FAU, 0x62C8A7F9U,
0xB602C312U, 0x44694011U, 0x5739B3E5U, 0xA55230E6U,
0xFB410CC2U, 0x092A8FC1U, 0x1A7A7C35U, 0xE811FF36U,
0x3CDB9BDDU, 0xCEB018DEU, 0xDDE0EB2AU, 0x2F8B6829U,
0x82F63B78U, 0x709DB87BU, 0x63CD4B8FU, 0x91A6C88CU,
0x456CAC67U, 0xB7072F64U, 0xA457DC90U, 0x563C5F93U,
0x082F63B7U, 0xFA44E0B4U, 0xE9141340U, 0x1B7F9043U,
0xCFB5F4A8U, 0x3DDE77ABU, 0x2E8E845FU, 0xDCE5075CU,
0x92A8FC17U, 0x60C37F14U, 0x73938CE0U, 0x81F80FE3U,
0x55326B08U, 0xA759E80BU, 0xB4091BFFU, 0x466298FCU,
0x1871A4D8U, 0xEA1A27DBU, 0xF94AD42FU, 0x0B21572CU,
0xDFEB33C7U, 0x2D80B0C4U, 0x3ED04330U, 0xCCBBC033U,
0xA24BB5A6U, 0x502036A5U, 0x4370C551U, 0xB11B4652U,
0x65D122B9U, 0x97BAA1BAU, 0x84EA524EU, 0x7681D14DU,
0x2892ED69U, 0xDAF96E6AU, 0xC9A99D9EU, 0x3BC21E9DU,
0xEF087A76U, 0x1D63F975U, 0x0E330A81U, 0xFC588982U,
0xB21572C9U, 0x407EF1CAU, 0x532E023EU, 0xA145813DU,
0x758FE5D6U, 0x87E466D5U, 0x94B49521U, 0x66DF1622U,
0x38CC2A06U, 0xCAA7A905U, 0xD9F75AF1U, 0x2B9CD9F2U,
0xFF56BD19U, 0x0D3D3E1AU, 0x1E6DCDEEU, 0xEC064EEDU,
0xC38D26C4U, 0x31E6A5C7U, 0x22B65633U, 0xD0DDD530U,
0x0417B1DBU, 0xF67C32D8U, 0xE52CC12CU, 0x1747422FU,
0x49547E0BU, 0xBB3FFD08U, 0xA86F0EFCU, 0x5A048DFFU,
0x8ECEE914U, 0x7CA56A17U, 0x6FF599E3U, 0x9D9E1AE0U,
0xD3D3E1ABU, 0x21B862A8U, 0x32E8915CU, 0xC083125FU,
0x144976B4U, 0xE622F5B7U, 0xF5720643U, 0x07198540U,
0x590AB964U, 0xAB613A67U, 0xB831C993U, 0x4A5A4A90U,
0x9E902E7BU, 0x6CFBAD78U, 0x7FAB5E8CU, 0x8DC0DD8FU,
0xE330A81AU, 0x115B2B19U, 0x020BD8EDU, 0xF0605BEEU,
0x24AA3F05U, 0xD6C1BC06U, 0xC5914FF2U, 0x37FACCF1U,
0x69E9F0D5U, 0x9B8273D6U, 0x88D28022U, 0x7AB90321U,
0xAE7367CAU, 0x5C18E4C9U, 0x4F48173DU, 0xBD23943EU,
0xF36E6F75U, 0x0105EC76U, 0x12551F82U, 0xE03E9C81U,
0x34F4F86AU, 0xC69F7B69U, 0xD5CF889DU, 0x27A40B9EU,
0x79B737BAU, 0x8BDCB4B9U, 0x988C474DU, 0x6AE7C44EU,
0xBE2DA0A5U, 0x4C4623A6U, 0x5F16D052U, 0xAD7D5351U,
};

static guint32
crc32c (const guint8* buf, unsigned int len, guint32 crc32_init)
{
    unsigned int i;
    guint32 crc;

    crc = crc32_init;
    for (i = 0; i < len; i++)
        CRC32C(crc, buf[i]);

    return crc;
}

static guint32
finalize_crc32c (guint32 crc)
{
    guint32 result;
    guint8 byte0,byte1,byte2,byte3;

    result = ~crc;
    byte0 = result & 0xff;
    byte1 = (result>>8) & 0xff;
    byte2 = (result>>16) & 0xff;
    byte3 = (result>>24) & 0xff;
    result = ((byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3);
    return result;
}

/*----------------------------------------------------------------------
 * Write current packet out
 */
static int
write_current_packet (gboolean cont)
{
    guint32  length         = 0;
    guint16  padding_length = 0;
    int      err;
    gboolean success;

    if (curr_offset > header_length) {
        /* Write the packet */

        /* Is direction indication on with an outbound packet? */
        gboolean isOutbound = has_direction && (direction == PACK_FLAGS_DIRECTION_OUTBOUND);

        /* Compute packet length */
        length = curr_offset;
        if (hdr_sctp) {
            padding_length = number_of_padding_bytes(length - header_length );
        } else {
            padding_length = 0;
        }
        /* Reset curr_offset, since we now write the headers */
        curr_offset = 0;

        /* Write Ethernet header */
        if (hdr_ethernet) {
            if (isOutbound)
            {
                memcpy(HDR_ETHERNET.dest_addr, hdr_eth_src_addr, 6);
                memcpy(HDR_ETHERNET.src_addr, hdr_eth_dest_addr, 6);
            } else {
                memcpy(HDR_ETHERNET.dest_addr, hdr_eth_dest_addr, 6);
                memcpy(HDR_ETHERNET.src_addr, hdr_eth_src_addr, 6);
            }
            HDR_ETHERNET.l3pid = g_htons(hdr_ethernet_proto);
            write_bytes((const char *)&HDR_ETHERNET, sizeof(HDR_ETHERNET));
        }

        /* Write IP header */
        if (hdr_ip) {
            if (isOutbound) {
                HDR_IP.src_addr = hdr_ip_dest_addr ? hdr_ip_dest_addr : IP_DST;
                HDR_IP.dest_addr = hdr_ip_src_addr? hdr_ip_src_addr : IP_SRC;
            }
            else {
                HDR_IP.src_addr = hdr_ip_src_addr? hdr_ip_src_addr : IP_SRC;
                HDR_IP.dest_addr = hdr_ip_dest_addr ? hdr_ip_dest_addr : IP_DST;
            }

            HDR_IP.packet_length = g_htons(length - ip_offset + padding_length);
            HDR_IP.protocol = (guint8) hdr_ip_proto;
            HDR_IP.hdr_checksum = 0;
            HDR_IP.hdr_checksum = in_checksum(&HDR_IP, sizeof(HDR_IP));
            write_bytes((const char *)&HDR_IP, sizeof(HDR_IP));

            /* initialize pseudo header for checksum calculation */
            pseudoh.src_addr    = HDR_IP.src_addr;
            pseudoh.dest_addr   = HDR_IP.dest_addr;
            pseudoh.zero        = 0;
            pseudoh.protocol    = (guint8) hdr_ip_proto;
            if (hdr_tcp) {
                pseudoh.length      = g_htons(length - header_length + sizeof(HDR_TCP));
            } else if (hdr_udp) {
                pseudoh.length      = g_htons(length - header_length + sizeof(HDR_UDP));
            }
        } else if (hdr_ipv6) {
            if (memcmp(isOutbound ? &hdr_ipv6_dest_addr : &hdr_ipv6_src_addr, &NO_IPv6_ADDRESS, sizeof(ws_in6_addr)))
                memcpy(&HDR_IPv6.ip6_src, isOutbound ? &hdr_ipv6_dest_addr : &hdr_ipv6_src_addr, sizeof(ws_in6_addr));
            if (memcmp(isOutbound ? &hdr_ipv6_src_addr : &hdr_ipv6_dest_addr, &NO_IPv6_ADDRESS, sizeof(ws_in6_addr)))
                memcpy(&HDR_IPv6.ip6_dst, isOutbound ? &hdr_ipv6_src_addr : &hdr_ipv6_dest_addr, sizeof(ws_in6_addr));

            HDR_IPv6.ip6_ctlun.ip6_un2_vfc &= 0x0F;
            HDR_IPv6.ip6_ctlun.ip6_un2_vfc |= (6<< 4);
            HDR_IPv6.ip6_ctlun.ip6_un1.ip6_un1_plen = g_htons(length - ip_offset - sizeof(HDR_IPv6) + padding_length);
            HDR_IPv6.ip6_ctlun.ip6_un1.ip6_un1_nxt  = (guint8) hdr_ip_proto;
            HDR_IPv6.ip6_ctlun.ip6_un1.ip6_un1_hlim = 32;
            write_bytes((const char *)&HDR_IPv6, sizeof(HDR_IPv6));

            /* initialize pseudo ipv6 header for checksum calculation */
            pseudoh6.src_addr6  = HDR_IPv6.ip6_src;
            pseudoh6.dst_addr6  = HDR_IPv6.ip6_dst;
            memset(pseudoh6.zero, 0, sizeof(pseudoh6.zero));
            pseudoh6.next_header   = (guint8) hdr_ip_proto;
            if (hdr_tcp) {
                pseudoh6.length      = g_htons(length - header_length + sizeof(HDR_TCP));
            } else if (hdr_udp) {
                pseudoh6.length      = g_htons(length - header_length + sizeof(HDR_UDP));
            }
        }

        /* Write UDP header */
        if (hdr_udp) {
            guint16 x16;
            guint32 u;

            /* initialize the UDP header */
            HDR_UDP.source_port = isOutbound ? g_htons(hdr_dest_port): g_htons(hdr_src_port);
            HDR_UDP.dest_port = isOutbound ? g_htons(hdr_src_port) : g_htons(hdr_dest_port);
            HDR_UDP.length      = hdr_ipv6 ? pseudoh6.length : pseudoh.length;
            HDR_UDP.checksum = 0;
            /* Note: g_ntohs()/g_htons() macro arg may be eval'd twice so calc value before invoking macro */
            x16  = hdr_ipv6 ? in_checksum(&pseudoh6, sizeof(pseudoh6)) : in_checksum(&pseudoh, sizeof(pseudoh));
            u    = g_ntohs(x16);
            x16  = in_checksum(&HDR_UDP, sizeof(HDR_UDP));
            u   += g_ntohs(x16);
            x16  = in_checksum(packet_buf + header_length, length - header_length);
            u   += g_ntohs(x16);
            x16  = (u & 0xffff) + (u>>16);
            HDR_UDP.checksum = g_htons(x16);
            if (HDR_UDP.checksum == 0) /* differentiate between 'none' and 0 */
                HDR_UDP.checksum = g_htons(1);
            write_bytes((const char *)&HDR_UDP, sizeof(HDR_UDP));
        }

        /* Write TCP header */
        if (hdr_tcp) {
            guint16 x16;
            guint32 u;

            /* initialize the TCP header */
            HDR_TCP.source_port = isOutbound ? g_htons(hdr_dest_port): g_htons(hdr_src_port);
            HDR_TCP.dest_port = isOutbound ? g_htons(hdr_src_port) : g_htons(hdr_dest_port);
            /* set ack number if we have direction */
            if (has_direction) {
                HDR_TCP.flags = 0x10;
                HDR_TCP.ack_num = g_ntohl(isOutbound ? tcp_out_seq_num : tcp_in_seq_num);
                HDR_TCP.ack_num = g_htonl(HDR_TCP.ack_num);
            }
            else {
                HDR_TCP.flags = 0;
                HDR_TCP.ack_num = 0;
            }
            HDR_TCP.seq_num = isOutbound ? tcp_in_seq_num : tcp_out_seq_num;
            HDR_TCP.window = g_htons(0x2000);
            HDR_TCP.checksum = 0;
            /* Note: g_ntohs()/g_htons() macro arg may be eval'd twice so calc value before invoking macro */
            x16  = hdr_ipv6 ? in_checksum(&pseudoh6, sizeof(pseudoh6)) : in_checksum(&pseudoh, sizeof(pseudoh));
            u    = g_ntohs(x16);
            x16  = in_checksum(&HDR_TCP, sizeof(HDR_TCP));
            u   += g_ntohs(x16);
            x16  = in_checksum(packet_buf + header_length, length - header_length);
            u   += g_ntohs(x16);
            x16  = (u & 0xffff) + (u>>16);
            HDR_TCP.checksum = g_htons(x16);
            if (HDR_TCP.checksum == 0) /* differentiate between 'none' and 0 */
                HDR_TCP.checksum = g_htons(1);
            write_bytes((const char *)&HDR_TCP, sizeof(HDR_TCP));
            if (isOutbound) {
                tcp_in_seq_num = g_ntohl(tcp_in_seq_num) + length - header_length;
                tcp_in_seq_num = g_htonl(tcp_in_seq_num);
            }
            else {
                tcp_out_seq_num = g_ntohl(tcp_out_seq_num) + length - header_length;
                tcp_out_seq_num = g_htonl(tcp_out_seq_num);
            }
        }

        /* Compute DATA chunk header */
        if (hdr_data_chunk) {
            hdr_data_chunk_bits = 0;
            if (packet_start == 0) {
                hdr_data_chunk_bits |= 0x02;
            }
            if (!cont) {
                hdr_data_chunk_bits |= 0x01;
            }
            HDR_DATA_CHUNK.type   = hdr_data_chunk_type;
            HDR_DATA_CHUNK.bits   = hdr_data_chunk_bits;
            HDR_DATA_CHUNK.length = g_htons(length - header_length + sizeof(HDR_DATA_CHUNK));
            HDR_DATA_CHUNK.tsn    = g_htonl(hdr_data_chunk_tsn);
            HDR_DATA_CHUNK.sid    = g_htons(hdr_data_chunk_sid);
            HDR_DATA_CHUNK.ssn    = g_htons(hdr_data_chunk_ssn);
            HDR_DATA_CHUNK.ppid   = g_htonl(hdr_data_chunk_ppid);
            hdr_data_chunk_tsn++;
            if (!cont) {
                hdr_data_chunk_ssn++;
            }
        }

        /* Write SCTP common header */
        if (hdr_sctp) {
            guint32 zero = 0;

            HDR_SCTP.src_port  = isOutbound ? g_htons(hdr_sctp_dest): g_htons(hdr_sctp_src);
            HDR_SCTP.dest_port = isOutbound ? g_htons(hdr_sctp_src) : g_htons(hdr_sctp_dest);
            HDR_SCTP.tag       = g_htonl(hdr_sctp_tag);
            HDR_SCTP.checksum  = g_htonl(0);
            HDR_SCTP.checksum  = crc32c((guint8 *)&HDR_SCTP, sizeof(HDR_SCTP), ~0);
            if (hdr_data_chunk) {
                HDR_SCTP.checksum  = crc32c((guint8 *)&HDR_DATA_CHUNK, sizeof(HDR_DATA_CHUNK), HDR_SCTP.checksum);
                HDR_SCTP.checksum  = crc32c((guint8 *)packet_buf + header_length, length - header_length, HDR_SCTP.checksum);
                HDR_SCTP.checksum  = crc32c((guint8 *)&zero, padding_length, HDR_SCTP.checksum);
            } else {
                HDR_SCTP.checksum  = crc32c((guint8 *)packet_buf + header_length, length - header_length, HDR_SCTP.checksum);
            }
            HDR_SCTP.checksum = finalize_crc32c(HDR_SCTP.checksum);
            HDR_SCTP.checksum  = g_htonl(HDR_SCTP.checksum);
            write_bytes((const char *)&HDR_SCTP, sizeof(HDR_SCTP));
        }

        /* Write DATA chunk header */
        if (hdr_data_chunk) {
            write_bytes((const char *)&HDR_DATA_CHUNK, sizeof(HDR_DATA_CHUNK));
        }

        /* Reset curr_offset, since we now write the trailers */
        curr_offset = length;

        /* Write DATA chunk padding */
        if (hdr_data_chunk && (padding_length > 0)) {
            memset(tempbuf, 0, padding_length);
            write_bytes((const char *)&tempbuf, padding_length);
            length += padding_length;
        }

        /* Write Ethernet trailer */
        if (hdr_ethernet && (length < 60)) {
            memset(tempbuf, 0, 60 - length);
            write_bytes((const char *)&tempbuf, 60 - length);
            length = 60;
        }
        if (use_pcapng) {
            success = pcapng_write_enhanced_packet_block(output_file,
                                                         NULL,
                                                         ts_sec, ts_nsec,
                                                         length, length,
                                                         0,
                                                         1000000000,
                                                         packet_buf,
                                                         (direction << PACK_FLAGS_DIRECTION_SHIFT),
                                                         &bytes_written, &err);
        } else {
            success = libpcap_write_packet(output_file,
                                           ts_sec, ts_nsec/1000,
                                           length, length,
                                           packet_buf,
                                           &bytes_written, &err);
        }
        if (!success) {
            fprintf(stderr, "File write error [%s] : %s\n",
                    output_filename, g_strerror(err));
            return EXIT_FAILURE;
        }
        if (ts_fmt == NULL) {
            /* fake packet counter */
            if (use_pcapng)
                ts_nsec++;
            else
                ts_nsec += 1000;
        }
        if (!quiet) {
            fprintf(stderr, "Wrote packet of %u bytes.\n", length);
        }
        num_packets_written++;
    }

    packet_start += curr_offset - header_length;
    curr_offset = header_length;
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------
 * Write file header and trailer
 */
static int
write_file_header (void)
{
    int      err;
    gboolean success;

    if (use_pcapng) {
        char *comment;
        GPtrArray *comments;

        comment = ws_strdup_printf("Generated from input file %s.", input_filename);
        comments = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(comments, comment);
        success = pcapng_write_section_header_block(output_file,
                                                    comments,
                                                    NULL,    /* HW */
                                                    NULL,    /* OS */
                                                    get_appname_and_version(),
                                                    -1,      /* section_length */
                                                    &bytes_written,
                                                    &err);
        g_ptr_array_free(comments, TRUE);
        if (success) {
            success = pcapng_write_interface_description_block(output_file,
                                                               NULL,
                                                               interface_name,
                                                               NULL,
                                                               "",
                                                               NULL,
                                                               NULL,
                                                               pcap_link_type,
                                                               WTAP_MAX_PACKET_SIZE_STANDARD,
                                                               &bytes_written,
                                                               0,
                                                               9,
                                                               &err);
        }
    } else {
        success = libpcap_write_file_header(output_file, pcap_link_type,
                                            WTAP_MAX_PACKET_SIZE_STANDARD, FALSE,
                                            &bytes_written, &err);
    }
    if (!success) {
        fprintf(stderr, "File write error [%s] : %s\n",
                output_filename, g_strerror(err));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------
 * Append a token to the packet preamble.
 */
static void
append_to_preamble (char *str)
{
    size_t toklen;

    if (packet_preamble_len != 0) {
        if (packet_preamble_len == PACKET_PREAMBLE_MAX_LEN)
            return; /* no room to add more preamble */
        /* Add a blank separator between the previous token and this token. */
        packet_preamble[packet_preamble_len++] = ' ';
    }
    toklen = strlen(str);
    if (toklen != 0) {
        if (packet_preamble_len + toklen > PACKET_PREAMBLE_MAX_LEN)
            return; /* no room to add the token to the preamble */
        (void) g_strlcpy(&packet_preamble[packet_preamble_len], str, PACKET_PREAMBLE_MAX_LEN);
        packet_preamble_len += (int) toklen;
        if (debug >= 2) {
            char *c;
            char xs[PACKET_PREAMBLE_MAX_LEN];
            (void) g_strlcpy(xs, packet_preamble, PACKET_PREAMBLE_MAX_LEN);
            while ((c = strchr(xs, '\r')) != NULL) *c=' ';
            fprintf (stderr, "[[append_to_preamble: \"%s\"]]", xs);
        }
    }
}

/*----------------------------------------------------------------------
 * Parse the preamble to get the timecode.
 */

static void
parse_preamble (void)
{
    struct tm  timecode;
    int        i;

     /*
     * Null-terminate the preamble.
     */
    packet_preamble[packet_preamble_len] = '\0';
    if (debug > 0)
        fprintf(stderr, "[[parse_preamble: \"%s\"]]\n", packet_preamble);

    if (has_direction) {
        switch (packet_preamble[0]) {
        case 'i':
        case 'I':
            direction = PACK_FLAGS_DIRECTION_INBOUND;
            packet_preamble[0] = ' ';
            break;
        case 'o':
        case 'O':
            direction = PACK_FLAGS_DIRECTION_OUTBOUND;
            packet_preamble[0] = ' ';
            break;
        default:
            direction = PACK_FLAGS_DIRECTION_UNKNOWN;
            break;
        }
        i = 0;
        while (packet_preamble[i] == ' ' ||
               packet_preamble[i] == '\r' ||
               packet_preamble[i] == '\t') {
            i++;
        }
        packet_preamble_len -= i;
        /* Also move the trailing '\0'. */
        memmove(packet_preamble, packet_preamble + i, packet_preamble_len + 1);
    }


    /*
     * If no "-t" flag was specified, don't attempt to parse the packet
     * preamble to extract a time stamp.
     */
    if (ts_fmt == NULL) {
        /* Clear Preamble */
        packet_preamble_len = 0;
        return;
    }

    /*
     * Initialize to today localtime, just in case not all fields
     * of the date and time are specified.
     */

    timecode = timecode_default;
    ts_nsec = 0;

    /* Ensure preamble has more than two chars before attempting to parse.
     * This should cover line breaks etc that get counted.
     */
    if (strlen(packet_preamble) > 2) {
        if (ts_fmt_iso) {
            nstime_t ts_iso;
            if (0 < iso8601_to_nstime(&ts_iso, packet_preamble, ISO8601_DATETIME_AUTO)) {
                ts_sec = ts_iso.secs;
                ts_nsec = ts_iso.nsecs;
            } else {
                ts_sec  = 0;  /* Jan 1,1970: 00:00 GMT; tshark/wireshark will display date/time as adjusted by timezone */
                ts_nsec = 0;
            }
        } else {
            char      *subsecs;
            char      *p;
            int        subseclen;

            /* Get Time leaving subseconds */
            subsecs = strptime( packet_preamble, ts_fmt, &timecode );
            if (subsecs != NULL) {
                /* Get the long time from the tm structure */
                /*  (will return -1 if failure)            */
                ts_sec  = mktime( &timecode );
            } else
                ts_sec = -1;    /* we failed to parse it */

            /* This will ensure incorrectly parsed dates get set to zero */
            if (-1 == ts_sec) {
                /* Sanitize - remove all '\r' */
                char *c;
                while ((c = strchr(packet_preamble, '\r')) != NULL) *c=' ';
                fprintf (stderr, "Failure processing time \"%s\" using time format \"%s\"\n   (defaulting to Jan 1,1970 00:00:00 GMT)\n",
                     packet_preamble, ts_fmt);
                if (debug >= 2) {
                    fprintf(stderr, "timecode: %02d/%02d/%d %02d:%02d:%02d %d\n",
                        timecode.tm_mday, timecode.tm_mon, timecode.tm_year,
                        timecode.tm_hour, timecode.tm_min, timecode.tm_sec, timecode.tm_isdst);
                }
                ts_sec  = 0;  /* Jan 1,1970: 00:00 GMT; tshark/wireshark will display date/time as adjusted by timezone */
                ts_nsec = 0;
            } else {
                /* Parse subseconds */
                ts_nsec = (guint32)strtol(subsecs, &p, 10);
                if (subsecs == p) {
                    /* Error */
                    ts_nsec = 0;
                } else {
                    /*
                     * Convert that number to a number
                     * of nanoseconds; if it's N digits
                     * long, it's in units of 10^(-N) seconds,
                     * so, to convert it to units of
                     * 10^-9 seconds, we multiply by
                     * 10^(9-N).
                     */
                    subseclen = (int) (p - subsecs);
                    if (subseclen > 9) {
                        /*
                         * *More* than 9 digits; 9-N is
                         * negative, so we divide by
                         * 10^(N-9).
                         */
                        for (i = subseclen - 9; i != 0; i--)
                            ts_nsec /= 10;
                    } else if (subseclen < 9) {
                        for (i = 9 - subseclen; i != 0; i--)
                            ts_nsec *= 10;
                    }
                }
            }
        }
    }
    if (debug >= 2) {
        char *c;
        while ((c = strchr(packet_preamble, '\r')) != NULL) *c=' ';
        fprintf(stderr, "[[parse_preamble: \"%s\"]]\n", packet_preamble);
        fprintf(stderr, "Format(%s), time(%u), subsecs(%u)\n", ts_fmt, (guint32)ts_sec, ts_nsec);
    }


    /* Clear Preamble */
    packet_preamble_len = 0;
}

/*----------------------------------------------------------------------
 * Start a new packet
 */
static int
start_new_packet (gboolean cont)
{
    if (debug >= 1)
        fprintf(stderr, "Start new packet (cont = %s).\n", cont ? "TRUE" : "FALSE");

    /* Write out the current packet, if required */
    if (write_current_packet(cont) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    num_packets_read++;

    /* Ensure we parse the packet preamble as it may contain the time */
    parse_preamble();

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------
 * Process a directive
 */
static void
process_directive (char *str)
{
    fprintf(stderr, "\n--- Directive [%s] currently unsupported ---\n", str + 10);
}

/*----------------------------------------------------------------------
 * Parse a single token (called from the scanner)
 */
int
parse_token (token_t token, char *str)
{
    guint32  num;
    int      by_eol;
    int      rollback = 0;
    int      line_size;
    int      i;
    char    *s2;
    char     tmp_str[3];

    /*
     * This is implemented as a simple state machine of five states.
     * State transitions are caused by tokens being received from the
     * scanner. The code should be self-documenting.
     */

    if (debug >= 2) {
        /* Sanitize - remove all '\r' */
        char *c;
        if (str!=NULL) { while ((c = strchr(str, '\r')) != NULL) *c=' '; }

        fprintf(stderr, "(%s, %s \"%s\") -> (",
                state_str[state], token_str[token], str ? str : "");
    }

    switch (state) {

    /* ----- Waiting for new packet -------------------------------------------*/
    case INIT:
        if (!str && token != T_EOL) goto fail_null_str;
        switch (token) {
        case T_TEXT:
            append_to_preamble(str);
            break;
        case T_DIRECTIVE:
            process_directive(str);
            break;
        case T_OFFSET:
            if (parse_num(str, TRUE, &num) != EXIT_SUCCESS)
                return EXIT_FAILURE;
            if (num == 0) {
                /* New packet starts here */
                if (start_new_packet(FALSE) != EXIT_SUCCESS)
                    return EXIT_FAILURE;
                state = READ_OFFSET;
                pkt_lnstart = packet_buf + num;
            }
            break;
        case T_EOL:
            /* Some describing text may be parsed as offset, but the invalid
               offset will be checked in the state of START_OF_LINE, so
               we add this transition to gain flexibility */
            state = START_OF_LINE;
            break;
        default:
            break;
        }
        break;

    /* ----- Processing packet, start of new line -----------------------------*/
    case START_OF_LINE:
        if (!str && token != T_EOL) goto fail_null_str;
        switch (token) {
        case T_TEXT:
            append_to_preamble(str);
            break;
        case T_DIRECTIVE:
            process_directive(str);
            break;
        case T_OFFSET:
            if (parse_num(str, TRUE, &num) != EXIT_SUCCESS)
                return EXIT_FAILURE;
            if (num == 0) {
                /* New packet starts here */
                if (start_new_packet(FALSE) != EXIT_SUCCESS)
                    return EXIT_FAILURE;
                packet_start = 0;
                state = READ_OFFSET;
            } else if ((num - packet_start) != curr_offset - header_length) {
                /*
                 * The offset we read isn't the one we expected.
                 * This may only mean that we mistakenly interpreted
                 * some text as byte values (e.g., if the text dump
                 * of packet data included a number with spaces around
                 * it).  If the offset is less than what we expected,
                 * assume that's the problem, and throw away the putative
                 * extra byte values.
                 */
                if (num < curr_offset) {
                    unwrite_bytes(curr_offset - num);
                    state = READ_OFFSET;
                } else {
                    /* Bad offset; switch to INIT state */
                    if (debug >= 1)
                        fprintf(stderr, "Inconsistent offset. Expecting %0X, got %0X. Ignoring rest of packet\n",
                                curr_offset, num);
                    if (write_current_packet(FALSE) != EXIT_SUCCESS)
                        return EXIT_FAILURE;
                    state = INIT;
                }
            } else {
                state = READ_OFFSET;
            }
            pkt_lnstart = packet_buf + num;
            break;
        case T_EOL:
            state = START_OF_LINE;
            break;
        default:
            break;
        }
        break;

    /* ----- Processing packet, read offset -----------------------------------*/
    case READ_OFFSET:
        switch (token) {
        case T_BYTE:
            /* Record the byte */
            state = READ_BYTE;
            if (!str) goto fail_null_str;
            if (write_byte(str) != EXIT_SUCCESS)
                return EXIT_FAILURE;
            break;
        case T_TEXT:
        case T_DIRECTIVE:
        case T_OFFSET:
            state = READ_TEXT;
            break;
        case T_EOL:
            state = START_OF_LINE;
            break;
        default:
            break;
        }
        break;

    /* ----- Processing packet, read byte -------------------------------------*/
    case READ_BYTE:
        switch (token) {
        case T_BYTE:
            /* Record the byte */
            if (write_byte(str) != EXIT_SUCCESS)
                return EXIT_FAILURE;
            break;
        case T_TEXT:
        case T_DIRECTIVE:
        case T_OFFSET:
        case T_EOL:
            by_eol = 0;
            state = READ_TEXT;
            if (token == T_EOL) {
                by_eol = 1;
                state = START_OF_LINE;
            }
            if (identify_ascii) {
                /* Here a line of pkt bytes reading is finished
                   compare the ascii and hex to avoid such situation:
                   "61 62 20 ab ", when ab is ascii dump then it should
                   not be treat as byte */
                rollback = 0;
                /* s2 is the ASCII string, s1 is the HEX string, e.g, when
                   s2 = "ab ", s1 = "616220"
                   we should find out the largest tail of s1 matches the head
                   of s2, it means the matched part in tail is the ASCII dump
                   of the head byte. These matched should be rollback */
                line_size = curr_offset-(int)(pkt_lnstart-packet_buf);
                s2 = (char*)g_malloc((line_size+1)/4+1);
                /* gather the possible pattern */
                for (i = 0; i < (line_size+1)/4; i++) {
                    tmp_str[0] = pkt_lnstart[i*3];
                    tmp_str[1] = pkt_lnstart[i*3+1];
                    tmp_str[2] = '\0';
                    /* it is a valid convertable string */
                    if (!g_ascii_isxdigit(tmp_str[0]) || !g_ascii_isxdigit(tmp_str[1])) {
                        break;
                    }
                    s2[i] = (char)strtoul(tmp_str, (char **)NULL, 16);
                    rollback++;
                    /* the 3rd entry is not a delimiter, so the possible byte pattern will not shown */
                    if (!(pkt_lnstart[i*3+2] == ' ')) {
                        if (by_eol != 1)
                            rollback--;
                        break;
                    }
                }
                /* If packet line start contains possible byte pattern, the line end
                   should contain the matched pattern if the user open the -a flag.
                   The packet will be possible invalid if the byte pattern cannot find
                   a matched one in the line of packet buffer.*/
                if (rollback > 0) {
                    if (strncmp(pkt_lnstart+line_size-rollback, s2, rollback) == 0) {
                        unwrite_bytes(rollback);
                    }
                    /* Not matched. This line contains invalid packet bytes, so
                       discard the whole line */
                    else {
                        unwrite_bytes(line_size);
                    }
                }
                g_free(s2);
            }
            break;
        default:
            break;
        }
        break;

    /* ----- Processing packet, read text -------------------------------------*/
    case READ_TEXT:
        switch (token) {
        case T_EOL:
            state = START_OF_LINE;
            break;
        default:
            break;
        }
        break;

    default:
        fprintf(stderr, "FATAL ERROR: Bad state (%d)", state);
        return EXIT_FAILURE;
    }

    if (debug >= 2)
        fprintf(stderr, ", %s)\n", state_str[state]);

    return EXIT_SUCCESS;

fail_null_str:
    fprintf(stderr, "FATAL ERROR: got NULL str pointer in state (%d)", state);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------
 * Print usage string and exit
 */
static void
print_usage (FILE *output)
{
    fprintf(output,
            "\n"
            "Usage: text2pcap [options] <infile> <outfile>\n"
            "\n"
            "where  <infile> specifies input  filename (use - for standard input)\n"
            "      <outfile> specifies output filename (use - for standard output)\n"
            "\n"
            "Input:\n"
            "  -o hex|oct|dec         parse offsets as (h)ex, (o)ctal or (d)ecimal;\n"
            "                         default is hex.\n"
            "  -t <timefmt>           treat the text before the packet as a date/time code;\n"
            "                         the specified argument is a format string of the sort\n"
            "                         supported by strptime.\n"
            "                         Example: The time \"10:15:14.5476\" has the format code\n"
            "                         \"%%H:%%M:%%S.\"\n"
            "                         NOTE: The subsecond component delimiter, '.', must be\n"
            "                         given, but no pattern is required; the remaining\n"
            "                         number is assumed to be fractions of a second.\n"
            "                         NOTE: Date/time fields from the current date/time are\n"
            "                         used as the default for unspecified fields.\n"
            "  -D                     the text before the packet starts with an I or an O,\n"
            "                         indicating that the packet is inbound or outbound.\n"
            "                         This is used when generating dummy headers.\n"
            "                         The indication is only stored if the output format is pcapng.\n"
            "  -a                     enable ASCII text dump identification.\n"
            "                         The start of the ASCII text dump can be identified\n"
            "                         and excluded from the packet data, even if it looks\n"
            "                         like a HEX dump.\n"
            "                         NOTE: Do not enable it if the input file does not\n"
            "                         contain the ASCII text dump.\n"
            "\n"
            "Output:\n"
            "  -l <typenum>           link-layer type number; default is 1 (Ethernet).  See\n"
            "                         https://www.tcpdump.org/linktypes.html for a list of\n"
            "                         numbers.  Use this option if your dump is a complete\n"
            "                         hex dump of an encapsulated packet and you wish to\n"
            "                         specify the exact type of encapsulation.\n"
            "                         Example: -l 7 for ARCNet packets.\n"
            "  -m <max-packet>        max packet length in output; default is %d\n"
            "  -n                     use pcapng instead of pcap as output format.\n"
            "  -N <intf-name>         assign name to the interface in the pcapng file.\n"
            "\n"
            "Prepend dummy header:\n"
            "  -e <l3pid>             prepend dummy Ethernet II header with specified L3PID\n"
            "                         (in HEX).\n"
            "                         Example: -e 0x806 to specify an ARP packet.\n"
            "  -i <proto>             prepend dummy IP header with specified IP protocol\n"
            "                         (in DECIMAL).\n"
            "                         Automatically prepends Ethernet header as well.\n"
            "                         Example: -i 46\n"
            "  -4 <srcip>,<destip>    prepend dummy IPv4 header with specified\n"
            "                         dest and source address.\n"
            "                         Example: -4 10.0.0.1,10.0.0.2\n"
            "  -6 <srcip>,<destip>    prepend dummy IPv6 header with specified\n"
            "                         dest and source address.\n"
            "                         Example: -6 fe80::202:b3ff:fe1e:8329,2001:0db8:85a3::8a2e:0370:7334\n"
            "  -u <srcp>,<destp>      prepend dummy UDP header with specified\n"
            "                         source and destination ports (in DECIMAL).\n"
            "                         Automatically prepends Ethernet & IP headers as well.\n"
            "                         Example: -u 1000,69 to make the packets look like\n"
            "                         TFTP/UDP packets.\n"
            "  -T <srcp>,<destp>      prepend dummy TCP header with specified\n"
            "                         source and destination ports (in DECIMAL).\n"
            "                         Automatically prepends Ethernet & IP headers as well.\n"
            "                         Example: -T 50,60\n"
            "  -s <srcp>,<dstp>,<tag> prepend dummy SCTP header with specified\n"
            "                         source/dest ports and verification tag (in DECIMAL).\n"
            "                         Automatically prepends Ethernet & IP headers as well.\n"
            "                         Example: -s 30,40,34\n"
            "  -S <srcp>,<dstp>,<ppi> prepend dummy SCTP header with specified\n"
            "                         source/dest ports and verification tag 0.\n"
            "                         Automatically prepends a dummy SCTP DATA\n"
            "                         chunk header with payload protocol identifier ppi.\n"
            "                         Example: -S 30,40,34\n"
            "\n"
            "Miscellaneous:\n"
            "  -h                     display this help and exit.\n"
            "  -v                     print version information and exit.\n"
            "  -d                     show detailed debug of parser states.\n"
            "  -q                     generate no output at all (automatically disables -d).\n"
            "",
            WTAP_MAX_PACKET_SIZE_STANDARD);
}

/*----------------------------------------------------------------------
 * Parse CLI options
 */
static int
parse_options (int argc, char *argv[])
{
    int   c;
    char *p;
    static const struct ws_option long_options[] = {
        {"help", ws_no_argument, NULL, 'h'},
        {"version", ws_no_argument, NULL, 'v'},
        {0, 0, 0, 0 }
    };
    struct tm *now_tm;

    /* Initialize the version information. */
    ws_init_version_info("Text2pcap (Wireshark)", NULL, NULL, NULL);

    /* Scan CLI parameters */
    while ((c = ws_getopt_long(argc, argv, "aDdhqe:i:l:m:nN:o:u:s:S:t:T:v4:6:", long_options, NULL)) != -1) {
        switch (c) {
        case 'h':
            show_help_header("Generate a capture file from an ASCII hexdump of packets.");
            print_usage(stdout);
            exit(0);
            break;
        case 'd': if (!quiet) debug++; break;
        case 'D': has_direction = TRUE; break;
        case 'q': quiet = TRUE; debug = 0; break;
        case 'l': pcap_link_type = (guint32)strtol(ws_optarg, NULL, 0); break;
        case 'm': max_offset = (guint32)strtol(ws_optarg, NULL, 0); break;
        case 'n': use_pcapng = TRUE; break;
        case 'N': interface_name = ws_optarg; break;
        case 'o':
            if (ws_optarg[0] != 'h' && ws_optarg[0] != 'o' && ws_optarg[0] != 'd') {
                fprintf(stderr, "Bad argument for '-o': %s\n", ws_optarg);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            switch (ws_optarg[0]) {
            case 'o': offset_base =  8; break;
            case 'h': offset_base = 16; break;
            case 'd': offset_base = 10; break;
            }
            break;
        case 'e':
            hdr_ethernet = TRUE;
            if (sscanf(ws_optarg, "%x", &hdr_ethernet_proto) < 1) {
                fprintf(stderr, "Bad argument for '-e': %s\n", ws_optarg);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            break;

        case 'i':
            hdr_ip_proto = strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || *p != '\0' || hdr_ip_proto < 0 ||
                  hdr_ip_proto > 255) {
                fprintf(stderr, "Bad argument for '-i': %s\n", ws_optarg);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            hdr_ethernet = TRUE;
            break;

        case 's':
            hdr_sctp = TRUE;
            hdr_data_chunk = FALSE;
            hdr_tcp = FALSE;
            hdr_udp = FALSE;
            hdr_sctp_src   = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad src port for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No dest port specified for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_sctp_dest = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad dest port for '-s'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No tag specified for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_sctp_tag = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || *p != '\0') {
                fprintf(stderr, "Bad tag for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }

            hdr_ip_proto = 132;
            hdr_ethernet = TRUE;
            break;
        case 'S':
            hdr_sctp = TRUE;
            hdr_data_chunk = TRUE;
            hdr_tcp = FALSE;
            hdr_udp = FALSE;
            hdr_sctp_src   = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad src port for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No dest port specified for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_sctp_dest = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad dest port for '-s'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No ppi specified for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_data_chunk_ppid = (guint32)strtoul(ws_optarg, &p, 10);
            if (p == ws_optarg || *p != '\0') {
                fprintf(stderr, "Bad ppi for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }

            hdr_ip_proto = 132;
            hdr_ethernet = TRUE;
            break;

        case 't':
            ts_fmt = ws_optarg;
            if (!strcmp(ws_optarg, "ISO"))
              ts_fmt_iso = 1;
            break;

        case 'u':
            hdr_udp = TRUE;
            hdr_tcp = FALSE;
            hdr_sctp = FALSE;
            hdr_data_chunk = FALSE;
            hdr_src_port = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad src port for '-u'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No dest port specified for '-u'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_dest_port = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || *p != '\0') {
                fprintf(stderr, "Bad dest port for '-u'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            hdr_ip_proto = 17;
            hdr_ethernet = TRUE;
            break;

        case 'T':
            hdr_tcp = TRUE;
            hdr_udp = FALSE;
            hdr_sctp = FALSE;
            hdr_data_chunk = FALSE;
            hdr_src_port = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || (*p != ',' && *p != '\0')) {
                fprintf(stderr, "Bad src port for '-T'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            if (*p == '\0') {
                fprintf(stderr, "No dest port specified for '-u'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            p++;
            ws_optarg = p;
            hdr_dest_port = (guint32)strtol(ws_optarg, &p, 10);
            if (p == ws_optarg || *p != '\0') {
                fprintf(stderr, "Bad dest port for '-T'\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            hdr_ip_proto = 6;
            hdr_ethernet = TRUE;
            break;

        case 'a':
            identify_ascii = TRUE;
            break;

        case 'v':
            show_version();
            exit(0);
            break;

        case '4':
        case '6':
            p = strchr(ws_optarg, ',');

            if (!p) {
                fprintf(stderr, "Bad source param addr for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }

            *p = '\0';
            if (c == '6')
            {
                hdr_ipv6 = TRUE;
                hdr_ip   = FALSE;
            }
            else
            {
                hdr_ip   = TRUE;
                hdr_ipv6 = FALSE;
            }
            hdr_ethernet = TRUE;

            if (hdr_ipv6 == TRUE) {
                if (!ws_inet_pton6(ws_optarg, &hdr_ipv6_src_addr)) {
                        fprintf(stderr, "Bad src addr -%c '%s'\n", c, p);
                        print_usage(stderr);
                        return EXIT_FAILURE;
                }
            } else {
                if (!ws_inet_pton4(ws_optarg, &hdr_ip_src_addr)) {
                        fprintf(stderr, "Bad src addr -%c '%s'\n", c, p);
                        print_usage(stderr);
                        return EXIT_FAILURE;
                }
            }

            p++;
            if (*p == '\0') {
                fprintf(stderr, "No dest addr specified for '-%c'\n", c);
                print_usage(stderr);
                return EXIT_FAILURE;
            }

            if (hdr_ipv6 == TRUE) {
                if (!ws_inet_pton6(p, &hdr_ipv6_dest_addr)) {
                        fprintf(stderr, "Bad dest addr for -%c '%s'\n", c, p);
                        print_usage(stderr);
                        return EXIT_FAILURE;
                }
            } else {
                if (!ws_inet_pton4(p, &hdr_ip_dest_addr)) {
                        fprintf(stderr, "Bad dest addr for -%c '%s'\n", c, p);
                        print_usage(stderr);
                        return EXIT_FAILURE;
                }
            }
            break;


        case '?':
        default:
            print_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (ws_optind >= argc || argc-ws_optind < 2) {
        fprintf(stderr, "Must specify input and output filename\n");
        print_usage(stderr);
        return EXIT_FAILURE;
    }

    if (max_offset > WTAP_MAX_PACKET_SIZE_STANDARD) {
        fprintf(stderr, "Maximum packet length cannot be more than %d bytes\n",
                WTAP_MAX_PACKET_SIZE_STANDARD);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[ws_optind], "-") != 0) {
        input_filename = argv[ws_optind];
        input_file = ws_fopen(input_filename, "rb");
        if (!input_file) {
            fprintf(stderr, "Cannot open file [%s] for reading: %s\n",
                    input_filename, g_strerror(errno));
            return EXIT_FAILURE;
        }
    } else {
        input_filename = "Standard input";
        input_file = stdin;
    }

    if (strcmp(argv[ws_optind+1], "-") != 0) {
        /* Write to a file.  Open the file, in binary mode. */
        output_filename = argv[ws_optind+1];
        output_file = ws_fopen(output_filename, "wb");
        if (!output_file) {
            fprintf(stderr, "Cannot open file [%s] for writing: %s\n",
                    output_filename, g_strerror(errno));
            return EXIT_FAILURE;
        }
    } else {
        /* Write to the standard output. */
#ifdef _WIN32
        /* Put the standard output in binary mode. */
        if (_setmode(1, O_BINARY) == -1) {
            /* "Should not happen" */
            fprintf(stderr, "Cannot put standard output in binary mode: %s\n",
                    g_strerror(errno));
            return EXIT_FAILURE;
        }
#endif
        output_filename = "Standard output";
        output_file = stdout;
    }

    /* Some validation */
    if (pcap_link_type != 1 && hdr_ethernet) {
        fprintf(stderr, "Dummy headers (-e, -i, -u, -s, -S -T) cannot be specified with link type override (-l)\n");
        return EXIT_FAILURE;
    }

    /* Set up our variables */
    if (!input_file) {
        input_file = stdin;
        input_filename = "Standard input";
    }
    if (!output_file) {
        output_file = stdout;
        output_filename = "Standard output";
    }

    ts_sec = time(0);               /* initialize to current time */
    now_tm = localtime(&ts_sec);
    if (now_tm == NULL) {
        /*
         * This shouldn't happen - on UN*X, this should Just Work, and
         * on Windows, it won't work if ts_sec is before the Epoch,
         * but it's long after 1970, so....
         */
        fprintf(stderr, "localtime (right now) failed\n");
        return EXIT_FAILURE;
    }
    timecode_default = *now_tm;
    timecode_default.tm_isdst = -1; /* Unknown for now, depends on time given to the strptime() function */

    if (hdr_ip_proto != -1 && !(hdr_ip || hdr_ipv6)) {
        /* If -i <proto> option is specified without -4 or -6 then add the default IPv4 header */
        hdr_ip = TRUE;
    }

    if (hdr_ip_proto == -1 && (hdr_ip || hdr_ipv6)) {
        /* if -4 or -6 option is specified without an IP protocol then fail */
        fprintf(stderr, "IP protocol requires a next layer protocol number\n");
        return EXIT_FAILURE;
    }

    if ((hdr_tcp || hdr_udp || hdr_sctp) && !(hdr_ip || hdr_ipv6)) {
        /*
         * If TCP (-T), UDP (-u) or SCTP (-s/-S) header options are specified
         * but none of IPv4 (-4) or IPv6 (-6) options then add an IPv4 header
         */
        hdr_ip = TRUE;
    }

    if (hdr_ip)
    {
        hdr_ethernet_proto = 0x0800;
    } else if (hdr_ipv6)
    {
        hdr_ethernet_proto = 0x86DD;
    }

    /* Display summary of our state */
    if (!quiet) {
        fprintf(stderr, "Input from: %s\n", input_filename);
        fprintf(stderr, "Output to: %s\n",  output_filename);
        fprintf(stderr, "Output format: %s\n", use_pcapng ? "pcapng" : "pcap");

        if (hdr_ethernet) fprintf(stderr, "Generate dummy Ethernet header: Protocol: 0x%0X\n",
                                  hdr_ethernet_proto);
        if (hdr_ip) fprintf(stderr, "Generate dummy IP header: Protocol: %ld\n",
                            hdr_ip_proto);
        if (hdr_ipv6) fprintf(stderr, "Generate dummy IPv6 header: Protocol: %ld\n",
                            hdr_ip_proto);
        if (hdr_udp) fprintf(stderr, "Generate dummy UDP header: Source port: %u. Dest port: %u\n",
                             hdr_src_port, hdr_dest_port);
        if (hdr_tcp) fprintf(stderr, "Generate dummy TCP header: Source port: %u. Dest port: %u\n",
                             hdr_src_port, hdr_dest_port);
        if (hdr_sctp) fprintf(stderr, "Generate dummy SCTP header: Source port: %u. Dest port: %u. Tag: %u\n",
                              hdr_sctp_src, hdr_sctp_dest, hdr_sctp_tag);
        if (hdr_data_chunk) fprintf(stderr, "Generate dummy DATA chunk header: TSN: %u. SID: %u. SSN: %u. PPID: %u\n",
                                    hdr_data_chunk_tsn, hdr_data_chunk_sid, hdr_data_chunk_ssn, hdr_data_chunk_ppid);
    }

    return EXIT_SUCCESS;
}

static void
text2pcap_vcmdarg_err(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

int
main(int argc, char *argv[])
{
    int ret = EXIT_SUCCESS;

    /* Initialize log handler early so we can have proper logging during startup. */
    ws_log_init("text2pcap", text2pcap_vcmdarg_err);

    /* Early logging command-line initialization. */
    ws_log_parse_args(&argc, argv, text2pcap_vcmdarg_err, 1);

#ifdef _WIN32
    create_app_running_mutex();
#endif /* _WIN32 */

    if (parse_options(argc, argv) != EXIT_SUCCESS) {
        ret = EXIT_FAILURE;
        goto clean_exit;
    }

    assert(input_file  != NULL);
    assert(output_file != NULL);

    if (write_file_header() != EXIT_SUCCESS) {
        ret = EXIT_FAILURE;
        goto clean_exit;
    }

    header_length = 0;
    if (hdr_ethernet) {
        header_length += (int)sizeof(HDR_ETHERNET);
    }
    if (hdr_ip) {
        ip_offset = header_length;
        header_length += (int)sizeof(HDR_IP);
    } else if (hdr_ipv6) {
        ip_offset = header_length;
        header_length += (int)sizeof(HDR_IPv6);
    }
    if (hdr_sctp) {
        header_length += (int)sizeof(HDR_SCTP);
    }
    if (hdr_data_chunk) {
        header_length += (int)sizeof(HDR_DATA_CHUNK);
    }
    if (hdr_tcp) {
        header_length += (int)sizeof(HDR_TCP);
    }
    if (hdr_udp) {
        header_length += (int)sizeof(HDR_UDP);
    }
    curr_offset = header_length;

    text2pcap_in = input_file;
    if (text2pcap_scan() == EXIT_SUCCESS) {
        if (write_current_packet(FALSE) != EXIT_SUCCESS)
            ret = EXIT_FAILURE;
    } else {
        ret = EXIT_FAILURE;
    }
    if (debug)
        fprintf(stderr, "\n-------------------------\n");
    if (!quiet) {
        fprintf(stderr, "Read %u potential packet%s, wrote %u packet%s (%" PRIu64 " byte%s).\n",
                num_packets_read, (num_packets_read == 1) ? "" : "s",
                num_packets_written, (num_packets_written == 1) ? "" : "s",
                bytes_written, (bytes_written == 1) ? "" : "s");
    }
clean_exit:
    if (input_file) {
        fclose(input_file);
    }
    if (output_file) {
        fclose(output_file);
    }
    return ret;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
