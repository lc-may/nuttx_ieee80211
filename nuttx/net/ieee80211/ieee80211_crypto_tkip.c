/****************************************************************************
 * net/ieee80211/ieee80211_cypto_tkip.c
 *
 * This code implements the Temporal Key Integrity Protocol (TKIP) defined
 * in IEEE Std 802.11-2007 section 8.3.2.
 *
 * Copyright (c) 2008 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <sys/socket.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <net/if.h>

#ifdef CONFIG_NET_ETHERNET
#  include <netinet/in.h>
#  include <nuttx/net/uip/uip.h>
#endif

#include <nuttx/kmalloc.h>
#include <nuttx/net/iob.h>

#include "ieee80211/ieee80211_var.h"
#include "ieee80211/ieee80211_crypto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef MIN
#  define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef uint8_t byte;           /* 8-bit byte (octet) */
typedef uint16_t u16b;          /* 16-bit unsigned word */
typedef uint32_t u32b;          /* 32-bit unsigned word */

static void Phase1(u16b *, const byte *, const byte *, u32b);
static void Phase2(byte *, const byte *, const u16b *, u16b);

/* TKIP software crypto context */

struct ieee80211_tkip_ctx
  {
    struct rc4_ctx rc4;
    const uint8_t *txmic;
    const uint8_t *rxmic;
    uint16_t txttak[5];
    uint16_t rxttak[5];
    uint8_t txttak_ok;
    uint8_t rxttak_ok;
  };

/* Initialize software crypto context.  This function can be overridden
 * by drivers doing hardware crypto.
 */

int ieee80211_tkip_set_key(struct ieee80211_s *ic, struct ieee80211_key *k)
{
  struct ieee80211_tkip_ctx *ctx;

  ctx = kmalloc(sizeof(struct ieee80211_tkip_ctx));
  if (ctx == NULL)
    {
      return -ENOMEM;
    }

  /* Use bits 128-191 as the Michael key for AA->SPA and bits 192-255 as the
   * Michael key for SPA->AA. */

#ifdef CONFIG_IEEE80211_AP
  if (ic->ic_opmode == IEEE80211_M_HOSTAP)
    {
      ctx->txmic = &k->k_key[16];
      ctx->rxmic = &k->k_key[24];
    }
  else
#endif
    {
      ctx->rxmic = &k->k_key[16];
      ctx->txmic = &k->k_key[24];
    }
  k->k_priv = ctx;
  return 0;
}

void ieee80211_tkip_delete_key(struct ieee80211_s *ic, struct ieee80211_key *k)
{
  if (k->k_priv != NULL)
    {
      kfree(k->k_priv);
    }

  k->k_priv = NULL;
}

/* pseudo-header used for TKIP MIC computation */

struct ieee80211_tkip_frame
  {
    uint8_t i_da[IEEE80211_ADDR_LEN];
    uint8_t i_sa[IEEE80211_ADDR_LEN];
    uint8_t i_pri;
    uint8_t i_pad[3];
  } packed_struct;

/* Compute TKIP MIC over an buffer chain starting "off" bytes from the
 * beginning.  This function should be kept independent from the software
 * TKIP crypto code so that drivers doing hardware crypto but not MIC can
 * call it without a software crypto context.
 */

void
ieee80211_tkip_mic(struct iob_s *m0, int off, const uint8_t * key,
                   uint8_t mic[IEEE80211_TKIP_MICLEN])
{
  const struct ieee80211_frame *wh;
  struct ieee80211_tkip_frame wht;
  MICHAEL_CTX ctx;              /* small enough */
  struct iob_s *iob;
  void *pos;
  int len;

  /* Assumes 802.11 header is contiguous */

  wh = (FAR struct ieee80211_frame *)IOB_DATA(m0);

  /* Construct pseudo-header for TKIP MIC computation */

  switch (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK)
    {
    case IEEE80211_FC1_DIR_NODS:
      IEEE80211_ADDR_COPY(wht.i_da, wh->i_addr1);
      IEEE80211_ADDR_COPY(wht.i_sa, wh->i_addr2);
      break;
    case IEEE80211_FC1_DIR_TODS:
      IEEE80211_ADDR_COPY(wht.i_da, wh->i_addr3);
      IEEE80211_ADDR_COPY(wht.i_sa, wh->i_addr2);
      break;
    case IEEE80211_FC1_DIR_FROMDS:
      IEEE80211_ADDR_COPY(wht.i_da, wh->i_addr1);
      IEEE80211_ADDR_COPY(wht.i_sa, wh->i_addr3);
      break;
    case IEEE80211_FC1_DIR_DSTODS:
      IEEE80211_ADDR_COPY(wht.i_da, wh->i_addr3);
      IEEE80211_ADDR_COPY(wht.i_sa,
                          ((const struct ieee80211_frame_addr4 *)wh)->i_addr4);
      break;
    }
  if (ieee80211_has_qos(wh))
    wht.i_pri = ieee80211_get_qos(wh) & IEEE80211_QOS_TID;
  else
    wht.i_pri = 0;
  wht.i_pad[0] = wht.i_pad[1] = wht.i_pad[2] = 0;

  michael_init(&ctx);
  michael_key(key, &ctx);

  michael_update(&ctx, (void *)&wht, sizeof(wht));

  iob = m0;

  /* Assumes the first "off" bytes are contiguous */

  pos = (FAR void *)IOB_DATA(iob) + off;
  len = iob->io_len - off;
  for (;;)
    {
      michael_update(&ctx, pos, len);
      if ((iob = iob->io_flink) == NULL)
        {
          break;
        }

      pos = (FAR void *)IOB_DATA(iob);
      len = iob->io_len;
    }

  michael_final(mic, &ctx);
}

/* shortcuts */
#define IEEE80211_TKIP_TAILLEN    \
    (IEEE80211_TKIP_MICLEN + IEEE80211_WEP_CRCLEN)
#define IEEE80211_TKIP_OVHD    \
    (IEEE80211_TKIP_HDRLEN + IEEE80211_TKIP_TAILLEN)

struct iob_s *ieee80211_tkip_encrypt(struct ieee80211_s *ic, struct iob_s *m0,
                                     struct ieee80211_key *k)
{
  struct ieee80211_tkip_ctx *ctx = k->k_priv;
  uint16_t wepseed[8];          /* needs to be 16-bit aligned for Phase2 */
  const struct ieee80211_frame *wh;
  uint8_t *ivp, *mic, *icvp;
  struct iob_s *next0, *iob, *next;
  uint32_t crc;
  int left, moff, noff, len, hdrlen;

  next0 = iob_alloc(false);
  if (next0 == NULL)
    {
      goto nospace;
    }

  if (iob_clone(next0, m0) < 0)
    {
      goto nospace;
    }

  next0->io_pktlen += IEEE80211_TKIP_HDRLEN;
  next0->io_len = CONFIG_IEEE80211_BUFSIZE;
  if (next0->io_len > next0->io_pktlen)
    {
      next0->io_len = next0->io_pktlen;
    }

  /* Copy 802.11 header */

  wh = (FAR struct ieee80211_frame *)IOB_DATA(m0);
  hdrlen = ieee80211_get_hdrlen(wh);
  memcpy(IOB_DATA(next0), wh, hdrlen);

  k->k_tsc++;                   /* increment the 48-bit TSC */

  /* Construct TKIP header */

  ivp = (FAR uint8_t *) IOB_DATA(next0) + hdrlen;
  ivp[0] = k->k_tsc >> 8;       /* TSC1 */

  /* WEP Seed = (TSC1 | 0x20) & 0x7f (see 8.3.2.2) */

  ivp[1] = (ivp[0] | 0x20) & 0x7f;
  ivp[2] = k->k_tsc;            /* TSC0 */
  ivp[3] = k->k_id << 6 | IEEE80211_WEP_EXTIV;  /* KeyID | ExtIV */
  ivp[4] = k->k_tsc >> 16;      /* TSC2 */
  ivp[5] = k->k_tsc >> 24;      /* TSC3 */
  ivp[6] = k->k_tsc >> 32;      /* TSC4 */
  ivp[7] = k->k_tsc >> 40;      /* TSC5 */

  /* compute WEP seed */
  if (!ctx->txttak_ok || (k->k_tsc & 0xffff) == 0)
    {
      Phase1(ctx->txttak, k->k_key, wh->i_addr2, k->k_tsc >> 16);
      ctx->txttak_ok = 1;
    }
  Phase2((uint8_t *) wepseed, k->k_key, ctx->txttak, k->k_tsc & 0xffff);
  rc4_keysetup(&ctx->rc4, (uint8_t *) wepseed, 16);

  /* encrypt frame body and compute WEP ICV */

  iob = m0;
  next = next0;
  moff = hdrlen;
  noff = hdrlen + IEEE80211_TKIP_HDRLEN;
  left = m0->io_pktlen - moff;
  crc = ~0;
  while (left > 0)
    {
      if (moff == iob->io_len)
        {
          /* Nothing left to copy from iob */

          iob = iob->io_flink;
          moff = 0;
        }

      if (noff == next->io_len)
        {
          struct iob_s *newbuf;

          /* next is full and there's more data to copy */

          newbuf = iob_alloc(false);
          if (newbuf == NULL)
            {
              goto nospace;
            }

          next->io_flink = newbuf;
          next = newbuf;
          next->io_len = 0;

          if (next->io_len > left)
            {
              next->io_len = left;
            }

          noff = 0;
        }

      len = MIN(iob->io_len - moff, next->io_len - noff);

      crc = ether_crc32_le_update(crc, IOB_DATA(iob) + moff, len);
      rc4_crypt(&ctx->rc4, IOB_DATA(iob) + moff, IOB_DATA(next) + noff, len);

      moff += len;
      noff += len;
      left -= len;
    }

  /* Reserve trailing space for TKIP MIC and WEP ICV */

  if (IOB_FREESPACE(next) < IEEE80211_TKIP_TAILLEN)
    {
      struct iob_s *newbuf;

      newbuf = iob_alloc(false);
      if (newbuf == NULL)
        {
          goto nospace;
        }

      next->io_flink = newbuf;
      next = newbuf;
      next->io_len = 0;
    }

  /* Compute TKIP MIC over clear text */

  mic = (FAR void *)IOB_DATA(next) + next->io_len;
  ieee80211_tkip_mic(m0, hdrlen, ctx->txmic, mic);
  crc = ether_crc32_le_update(crc, mic, IEEE80211_TKIP_MICLEN);
  rc4_crypt(&ctx->rc4, mic, mic, IEEE80211_TKIP_MICLEN);
  next->io_len += IEEE80211_TKIP_MICLEN;

  /* Finalize WEP ICV */

  icvp = (FAR void *)IOB_DATA(next) + next->io_len;
  crc = ~crc;
  icvp[0] = crc;
  icvp[1] = crc >> 8;
  icvp[2] = crc >> 16;
  icvp[3] = crc >> 24;
  rc4_crypt(&ctx->rc4, icvp, icvp, IEEE80211_WEP_CRCLEN);
  next->io_len += IEEE80211_WEP_CRCLEN;

  next0->io_pktlen += IEEE80211_TKIP_TAILLEN;

  iob_free_chain(m0);
  return next0;

nospace:
  iob_free_chain(m0);
  if (next0 != NULL)
    {
      iob_free_chain(next0);
    }

  return NULL;
}

struct iob_s *ieee80211_tkip_decrypt(struct ieee80211_s *ic, struct iob_s *m0,
                                     struct ieee80211_key *k)
{
  struct ieee80211_tkip_ctx *ctx = k->k_priv;
  struct ieee80211_frame *wh;
  uint16_t wepseed[8];          /* needs to be 16-bit aligned for Phase2 */
  uint8_t buf[IEEE80211_TKIP_MICLEN + IEEE80211_WEP_CRCLEN];
  uint8_t mic[IEEE80211_TKIP_MICLEN];
  uint64_t tsc, *prsc;
  uint32_t crc, crc0;
  uint8_t *ivp, *mic0;
  uint8_t tid;
  struct iob_s *next0, *iob, *next;
  int hdrlen, left, moff, noff, len;

  wh = (FAR struct ieee80211_frame *)IOB_DATA(m0);
  hdrlen = ieee80211_get_hdrlen(wh);

  if (m0->io_pktlen < hdrlen + IEEE80211_TKIP_OVHD)
    {
      iob_free_chain(m0);
      return NULL;
    }

  ivp = (uint8_t *) wh + hdrlen;

  /* check that ExtIV bit is set */

  if (!(ivp[3] & IEEE80211_WEP_EXTIV))
    {
      iob_free_chain(m0);
      return NULL;
    }

  /* retrieve last seen packet number for this frame priority */

  tid = ieee80211_has_qos(wh) ? ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;
  prsc = &k->k_rsc[tid];

  /* extract the 48-bit TSC from the TKIP header */

  tsc = (uint64_t) ivp[2] |
    (uint64_t) ivp[0] << 8 |
    (uint64_t) ivp[4] << 16 |
    (uint64_t) ivp[5] << 24 | (uint64_t) ivp[6] << 32 | (uint64_t) ivp[7] << 40;

  if (tsc <= *prsc)
    {
      /* Replayed frame, discard */

      iob_free_chain(m0);
      return NULL;
    }

  next0 = iob_alloc(false);
  if (next0 == NULL)
    {
      goto nospace;
    }

  if (iob_clone(next0, m0) < 0)
    {
      goto nospace;
    }

  next0->io_pktlen -= IEEE80211_TKIP_OVHD;
  next0->io_len = CONFIG_IEEE80211_BUFSIZE;

  if (next0->io_len > next0->io_pktlen)
    {
      next0->io_len = next0->io_pktlen;
    }

  /* Copy 802.11 header and clear protected bit */

  memcpy(IOB_DATA(next0), wh, hdrlen);
  wh = (FAR struct ieee80211_frame *)IOB_DATA(next0);
  wh->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;

  /* compute WEP seed */

  if (!ctx->rxttak_ok || (tsc >> 16) != (*prsc >> 16))
    {
      ctx->rxttak_ok = 0;       /* invalidate cached TTAK (if any) */
      Phase1(ctx->rxttak, k->k_key, wh->i_addr2, tsc >> 16);
    }
  Phase2((uint8_t *) wepseed, k->k_key, ctx->rxttak, tsc & 0xffff);
  rc4_keysetup(&ctx->rc4, (uint8_t *) wepseed, 16);

  /* decrypt frame body and compute WEP ICV */

  iob = m0;
  next = next0;
  moff = hdrlen + IEEE80211_TKIP_HDRLEN;
  noff = hdrlen;
  left = next0->io_pktlen - noff;
  crc = ~0;
  while (left > 0)
    {
      if (moff == iob->io_len)
        {
          /* Nothing left to copy from iob */

          iob = iob->io_flink;
          moff = 0;
        }

      if (noff == next->io_len)
        {
          struct iob_s *newbuf;

          /* next is full and there's more data to copy */

          newbuf = iob_alloc(false);
          if (newbuf == NULL)
            {
              goto nospace;
            }

          next->io_flink = newbuf;
          next = newbuf;
          next->io_len = 0;

          if (next->io_len > left)
            {
              next->io_len = left;
            }

          noff = 0;
        }

      len = MIN(iob->io_len - moff, next->io_len - noff);

      rc4_crypt(&ctx->rc4, IOB_DATA(iob) + moff, IOB_DATA(next) + noff, len);
      crc = ether_crc32_le_update(crc, IOB_DATA(next) + noff, len);

      moff += len;
      noff += len;
      left -= len;
    }

  /* Extract and decrypt TKIP MIC and WEP ICV from m0's tail */

  iob_copyout(buf, iob, moff, IEEE80211_TKIP_TAILLEN);
  rc4_crypt(&ctx->rc4, buf, buf, IEEE80211_TKIP_TAILLEN);

  /* Include TKIP MIC in WEP ICV */

  mic0 = buf;
  crc = ether_crc32_le_update(crc, mic0, IEEE80211_TKIP_MICLEN);
  crc = ~crc;

  /* Decrypt ICV and compare it with calculated ICV */

  crc0 = *(uint32_t *) (buf + IEEE80211_TKIP_MICLEN);
  if (crc != letoh32(crc0))
    {
      iob_free_chain(m0);
      iob_free_chain(next0);
      return NULL;
    }

  /* Compute TKIP MIC over decrypted message */

  ieee80211_tkip_mic(next0, hdrlen, ctx->rxmic, mic);

  /* Check that it matches the MIC in received frame */

  if (memcmp(mic0, mic, IEEE80211_TKIP_MICLEN) != 0)
    {
      iob_free_chain(m0);
      iob_free_chain(next0);
      ieee80211_michael_mic_failure(ic, tsc);
      return NULL;
    }

  /* update last seen packet number (MIC is validated) */

  *prsc = tsc;

  /* mark cached TTAK as valid */

  ctx->rxttak_ok = 1;

  iob_free_chain(m0);
  return next0;

nospace:
  iob_free_chain(m0);
  if (next0 != NULL)
    {
      iob_free_chain(next0);
    }

  return NULL;
}

#ifdef CONFIG_IEEE80211_AP

/* This function is called in HostAP mode to deauthenticate all STAs using
 * TKIP as their pairwise or group cipher (as part of TKIP countermeasures).
 */

static void ieee80211_tkip_deauth(void *arg, struct ieee80211_node *ni)
{
  struct ieee80211_s *ic = arg;

  if (ni->ni_state == IEEE80211_STA_ASSOC &&
      (ic->ic_bss->ni_rsngroupcipher == IEEE80211_CIPHER_TKIP ||
       ni->ni_rsncipher == IEEE80211_CIPHER_TKIP))
    {
      /* deauthenticate STA */

      IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_DEAUTH,
                          IEEE80211_REASON_MIC_FAILURE);
      ieee80211_node_leave(ic, ni);
    }
}
#endif                                 /* CONFIG_IEEE80211_AP */

/* This function can be called by the software TKIP crypto code or by the
 * drivers when their hardware crypto engines detect a Michael MIC failure.
 */

void ieee80211_michael_mic_failure(struct ieee80211_s *ic, uint64_t tsc)
{
  extern int ticks;

  if (ic->ic_flags & IEEE80211_F_COUNTERM)
    return;                     /* countermeasures already active */

  ndbg("ERROR: %s: Michael MIC failure\n", ic->ic_ifname);

  /* NB. do not send Michael MIC Failure reports as recommended since these may 
   * be used as an oracle to verify CRC guesses as described in Beck, M. and
   * Tews S. "Practical attacks against WEP and WPA"
   * http://dl.aircrack-ng.org/breakingwepandwpa.pdf */

  /* Activate TKIP countermeasures (see 8.3.2.4) if less than 60 seconds have
   * passed since the most recent previous MIC failure. */

  if (ic->ic_tkip_micfail == 0 || ticks - (ic->ic_tkip_micfail + 60 * hz) >= 0)
    {
      ic->ic_tkip_micfail = ticks;
      ic->ic_tkip_micfail_last_tsc = tsc;
      return;
    }

  switch (ic->ic_opmode)
    {
#ifdef CONFIG_IEEE80211_AP
    case IEEE80211_M_HOSTAP:
      /* refuse new TKIP associations for the next 60 seconds */
      ic->ic_flags |= IEEE80211_F_COUNTERM;

      /* deauthenticate all currently associated STAs using TKIP */
      ieee80211_iterate_nodes(ic, ieee80211_tkip_deauth, ic);
      break;
#endif
    case IEEE80211_M_STA:
      /* 
       * Notify the AP of MIC failures: send two Michael
       * MIC Failure Report frames back-to-back to trigger
       * countermeasures at the AP end.
       */
      (void)ieee80211_send_eapol_key_req(ic, ic->ic_bss,
                                         EAPOL_KEY_KEYMIC | EAPOL_KEY_ERROR |
                                         EAPOL_KEY_SECURE,
                                         ic->ic_tkip_micfail_last_tsc);
      (void)ieee80211_send_eapol_key_req(ic, ic->ic_bss,
                                         EAPOL_KEY_KEYMIC | EAPOL_KEY_ERROR |
                                         EAPOL_KEY_SECURE, tsc);

      /* deauthenticate from the AP.. */
      IEEE80211_SEND_MGMT(ic, ic->ic_bss,
                          IEEE80211_FC0_SUBTYPE_DEAUTH,
                          IEEE80211_REASON_MIC_FAILURE);
      /* ..and find another one */
      (void)ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
      break;
    default:
      break;
    }

  ic->ic_tkip_micfail = ticks;
  ic->ic_tkip_micfail_last_tsc = tsc;
}

/***********************************************************************
   Contents:    Generate IEEE 802.11 per-frame RC4 key hash test vectors
   Date:        April 19, 2002
   Notes:
   This code is written for pedagogical purposes, NOT for performance.
************************************************************************/

/* macros for extraction/creation of byte/u16b values */

#define RotR1(v16)    ((((v16) >> 1) & 0x7FFF) ^ (((v16) & 1) << 15))
#define   Lo8(v16)    ((byte)( (v16)       & 0x00FF))
#define   Hi8(v16)    ((byte)(((v16) >> 8) & 0x00FF))
#define Lo16(v32)    ((u16b)( (v32)       & 0xFFFF))
#define Hi16(v32)    ((u16b)(((v32) >>16) & 0xFFFF))
#define Mk16(hi,lo)    ((lo) ^ (((u16b)(hi)) << 8))

/* select the Nth 16-bit word of the Temporal Key byte array TK[] */

#define TK16(N)        Mk16(TK[2 * (N) + 1], TK[2 * (N)])

/* S-box lookup: 16 bits --> 16 bits */

#define _S_(v16)    (Sbox[Lo8(v16)] ^ swap16(Sbox[Hi8(v16)]))

/* fixed algorithm "parameters" */

#define PHASE1_LOOP_CNT     8   /* this needs to be "big enough" */
#define TA_SIZE         6       /* 48-bit transmitter address */
#define TK_SIZE        16       /* 128-bit Temporal Key */
#define P1K_SIZE    10          /* 80-bit Phase1 key */
#define RC4_KEY_SIZE    16      /* 128-bit RC4KEY (104 bits unknown) */

/* 2-byte by 2-byte subset of the full AES S-box table */

static const u16b Sbox[256] =   /* Sbox for hash */
{
  0xC6A5, 0xF884, 0xEE99, 0xF68D, 0xFF0D, 0xD6BD, 0xDEB1, 0x9154,
  0x6050, 0x0203, 0xCEA9, 0x567D, 0xE719, 0xB562, 0x4DE6, 0xEC9A,
  0x8F45, 0x1F9D, 0x8940, 0xFA87, 0xEF15, 0xB2EB, 0x8EC9, 0xFB0B,
  0x41EC, 0xB367, 0x5FFD, 0x45EA, 0x23BF, 0x53F7, 0xE496, 0x9B5B,
  0x75C2, 0xE11C, 0x3DAE, 0x4C6A, 0x6C5A, 0x7E41, 0xF502, 0x834F,
  0x685C, 0x51F4, 0xD134, 0xF908, 0xE293, 0xAB73, 0x6253, 0x2A3F,
  0x080C, 0x9552, 0x4665, 0x9D5E, 0x3028, 0x37A1, 0x0A0F, 0x2FB5,
  0x0E09, 0x2436, 0x1B9B, 0xDF3D, 0xCD26, 0x4E69, 0x7FCD, 0xEA9F,
  0x121B, 0x1D9E, 0x5874, 0x342E, 0x362D, 0xDCB2, 0xB4EE, 0x5BFB,
  0xA4F6, 0x764D, 0xB761, 0x7DCE, 0x527B, 0xDD3E, 0x5E71, 0x1397,
  0xA6F5, 0xB968, 0x0000, 0xC12C, 0x4060, 0xE31F, 0x79C8, 0xB6ED,
  0xD4BE, 0x8D46, 0x67D9, 0x724B, 0x94DE, 0x98D4, 0xB0E8, 0x854A,
  0xBB6B, 0xC52A, 0x4FE5, 0xED16, 0x86C5, 0x9AD7, 0x6655, 0x1194,
  0x8ACF, 0xE910, 0x0406, 0xFE81, 0xA0F0, 0x7844, 0x25BA, 0x4BE3,
  0xA2F3, 0x5DFE, 0x80C0, 0x058A, 0x3FAD, 0x21BC, 0x7048, 0xF104,
  0x63DF, 0x77C1, 0xAF75, 0x4263, 0x2030, 0xE51A, 0xFD0E, 0xBF6D,
  0x814C, 0x1814, 0x2635, 0xC32F, 0xBEE1, 0x35A2, 0x88CC, 0x2E39,
  0x9357, 0x55F2, 0xFC82, 0x7A47, 0xC8AC, 0xBAE7, 0x322B, 0xE695,
  0xC0A0, 0x1998, 0x9ED1, 0xA37F, 0x4466, 0x547E, 0x3BAB, 0x0B83,
  0x8CCA, 0xC729, 0x6BD3, 0x283C, 0xA779, 0xBCE2, 0x161D, 0xAD76,
  0xDB3B, 0x6456, 0x744E, 0x141E, 0x92DB, 0x0C0A, 0x486C, 0xB8E4,
  0x9F5D, 0xBD6E, 0x43EF, 0xC4A6, 0x39A8, 0x31A4, 0xD337, 0xF28B,
  0xD532, 0x8B43, 0x6E59, 0xDAB7, 0x018C, 0xB164, 0x9CD2, 0x49E0,
  0xD8B4, 0xACFA, 0xF307, 0xCF25, 0xCAAF, 0xF48E, 0x47E9, 0x1018,
  0x6FD5, 0xF088, 0x4A6F, 0x5C72, 0x3824, 0x57F1, 0x73C7, 0x9751,
  0xCB23, 0xA17C, 0xE89C, 0x3E21, 0x96DD, 0x61DC, 0x0D86, 0x0F85,
  0xE090, 0x7C42, 0x71C4, 0xCCAA, 0x90D8, 0x0605, 0xF701, 0x1C12,
  0xC2A3, 0x6A5F, 0xAEF9, 0x69D0, 0x1791, 0x9958, 0x3A27, 0x27B9,
  0xD938, 0xEB13, 0x2BB3, 0x2233, 0xD2BB, 0xA970, 0x0789, 0x33A7,
  0x2DB6, 0x3C22, 0x1592, 0xC920, 0x8749, 0xAAFF, 0x5078, 0xA57A,
  0x038F, 0x59F8, 0x0980, 0x1A17, 0x65DA, 0xD731, 0x84C6, 0xD0B8,
  0x82C3, 0x29B0, 0x5A77, 0x1E11, 0x7BCB, 0xA8FC, 0x6DD6, 0x2C3A
};

/**********************************************************************
 * Routine: Phase 1 -- generate P1K, given TA, TK, IV32
 *
 * Inputs:
 *     TK[]      = Temporal Key                         [128 bits]
 *     TA[]      = transmitter's MAC address            [ 48 bits]
 *     IV32      = upper 32 bits of IV                  [ 32 bits]
 * Output:
 *     P1K[]     = Phase 1 key                          [ 80 bits]
 *
 * Note:
 *     This function only needs to be called every 2**16 frames,
 *     although in theory it could be called every frame.
 *
 **********************************************************************/

static void Phase1(u16b * P1K, const byte * TK, const byte * TA, u32b IV32)
{
  int i;

  /* Initialize the 80 bits of P1K[] from IV32 and TA[0..5] */
  P1K[0] = Lo16(IV32);
  P1K[1] = Hi16(IV32);
  P1K[2] = Mk16(TA[1], TA[0]);  /* use TA[] as little-endian */
  P1K[3] = Mk16(TA[3], TA[2]);
  P1K[4] = Mk16(TA[5], TA[4]);

  /* Now compute an unbalanced Feistel cipher with 80-bit block */
  /* size on the 80-bit block P1K[], using the 128-bit key TK[] */
  for (i = 0; i < PHASE1_LOOP_CNT; i++)
    {
      /* Each add operation here is mod 2**16 */
      P1K[0] += _S_(P1K[4] ^ TK16((i & 1) + 0));
      P1K[1] += _S_(P1K[0] ^ TK16((i & 1) + 2));
      P1K[2] += _S_(P1K[1] ^ TK16((i & 1) + 4));
      P1K[3] += _S_(P1K[2] ^ TK16((i & 1) + 6));
      P1K[4] += _S_(P1K[3] ^ TK16((i & 1) + 0));
      P1K[4] += i;              /* avoid "slide attacks" */
    }
}

/**********************************************************************
 * Routine: Phase 2 -- generate RC4KEY, given TK, P1K, IV16
 *
 * Inputs:
 *     TK[]      = Temporal Key                         [128 bits]
 *     P1K[]     = Phase 1 output key                   [ 80 bits]
 *     IV16      = low 16 bits of IV counter            [ 16 bits]
 * Output:
 *     RC4KEY[] = the key used to encrypt the frame     [128 bits]
 *
 * Note:
 *     The value {TA,IV32,IV16} for Phase1/Phase2 must be unique
 *     across all frames using the same key TK value. Then, for a
 *     given value of TK[], this TKIP48 construction guarantees that
 *     the final RC4KEY value is unique across all frames.
 *
 **********************************************************************/

static void Phase2(byte * RC4KEY, const byte * TK, const u16b * P1K, u16b IV16)
{
  u16b *PPK;                    /* temporary key for mixing */
  int i;

  /* Suggested implementation optimization: if PPK[] is "overlaid"
   * appropriately on RC4KEY[], there is no need for the final for
   * loop that copies the PPK[] result into RC4KEY[].
   */

  PPK = (u16b *) & RC4KEY[4];

  /* all adds in the PPK[] equations below are mod 2**16 */

  for (i = 0; i < 5; i++)
    PPK[i] = P1K[i];            /* first, copy P1K to PPK */
  PPK[5] = P1K[4] + IV16;       /* next, add in IV16 */

  /* Bijective non-linear mixing of the 96 bits of PPK[0..5] */

  PPK[0] += _S_(PPK[5] ^ TK16(0));      /* Mix key in each "round" */
  PPK[1] += _S_(PPK[0] ^ TK16(1));
  PPK[2] += _S_(PPK[1] ^ TK16(2));
  PPK[3] += _S_(PPK[2] ^ TK16(3));
  PPK[4] += _S_(PPK[3] ^ TK16(4));
  PPK[5] += _S_(PPK[4] ^ TK16(5));      /* Total # S-box lookups == 6 */

  /* Final sweep: bijective, linear. Rotates kill LSB correlations */

  PPK[0] += RotR1(PPK[5] ^ TK16(6));
  PPK[1] += RotR1(PPK[0] ^ TK16(7));    /* Use all of TK[] in Phase2 */
  PPK[2] += RotR1(PPK[1]);
  PPK[3] += RotR1(PPK[2]);
  PPK[4] += RotR1(PPK[3]);
  PPK[5] += RotR1(PPK[4]);

  /* At this point, for a given key TK[0..15], the 96-bit output */
  /* value PPK[0..5] is guaranteed to be unique, as a function */
  /* of the 96-bit "input" value {TA,IV32,IV16}. That is, P1K */
  /* is now a keyed permutation of {TA,IV32,IV16}. */
  /* Set RC4KEY[0..3], which includes cleartext portion of RC4 key */

  RC4KEY[0] = Hi8(IV16);        /* RC4KEY[0..2] is the WEP IV */
  RC4KEY[1] = (Hi8(IV16) | 0x20) & 0x7F;        /* Help avoid FMS weak keys */
  RC4KEY[2] = Lo8(IV16);
  RC4KEY[3] = Lo8((PPK[5] ^ TK16(0)) >> 1);

#ifdef CONFIG_ENDIAN_BIG
  /* Copy 96 bits of PPK[0..5] to RC4KEY[4..15] (little-endian) */

  for (i = 0; i < 6; i++)
    PPK[i] = swap16(PPK[i]);
#endif
}
