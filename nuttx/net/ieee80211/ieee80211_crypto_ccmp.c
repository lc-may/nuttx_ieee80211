/****************************************************************************
 * net/ieee80211/ieee80211_crypto-ccmp.c
 *
 * This code implements the CTR with CBC-MAC protocol (CCMP) defined in
 * IEEE Std 802.11-2007 section 8.3.3.
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
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/socket.h>

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <net/if.h>

#ifdef CONFIG_NET_ETHERNET
#  include <netinet/in.h>
#  include <nuttx/net/uip/uip.h>
#endif

#include <nuttx/kmalloc.h>
#include <nuttx/net/iob.h>

#include "ieee80211/ieee80211_ifnet.h"
#include "ieee80211/ieee80211_var.h"
#include "ieee80211/ieee80211_crypto.h"

/* CCMP software crypto context */

struct ieee80211_ccmp_ctx
  {
    rijndael_ctx rijndael;
  };

/* Initialize software crypto context.  This function can be overridden
 * by drivers doing hardware crypto.
 */

int ieee80211_ccmp_set_key(struct ieee80211_s *ic, struct ieee80211_key *k)
{
  struct ieee80211_ccmp_ctx *ctx;

  ctx = kmalloc(sizeof(*ctx));
  if (ctx == NULL)
    {
      return -ENOMEM;
    }

  rijndael_set_key_enc_only(&ctx->rijndael, k->k_key, 128);
  k->k_priv = ctx;
  return 0;
}

void ieee80211_ccmp_delete_key(struct ieee80211_s *ic, struct ieee80211_key *k)
{
  if (k->k_priv != NULL)
    {
      kfree(k->k_priv);
    }

  k->k_priv = NULL;
}

/* Counter with CBC-MAC (CCM) - see RFC3610.
 * CCMP uses the following CCM parameters: M = 8, L = 2
 */

static void ieee80211_ccmp_phase1(rijndael_ctx * ctx,
                                  const struct ieee80211_frame *wh, uint64_t pn,
                                  int lm, uint8_t b[16], uint8_t a[16],
                                  uint8_t s0[16])
{
  uint8_t auth[32], nonce[13];
  uint8_t *aad;
  uint8_t tid = 0;
  int la, i;

  /* construct AAD (additional authenticated data) */
  aad = &auth[2];               /* skip l(a), will be filled later */
  *aad = wh->i_fc[0];
  /* 11w: conditionnally mask subtype field */
  if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA)
    *aad &= ~IEEE80211_FC0_SUBTYPE_MASK;
  aad++;
  /* protected bit is already set in wh */
  *aad = wh->i_fc[1];
  *aad &= ~(IEEE80211_FC1_RETRY | IEEE80211_FC1_PWR_MGT |
            IEEE80211_FC1_MORE_DATA);
  /* 11n: conditionnally mask order bit */
  if (ieee80211_has_htc(wh))
    *aad &= ~IEEE80211_FC1_ORDER;
  aad++;
  IEEE80211_ADDR_COPY(aad, wh->i_addr1);
  aad += IEEE80211_ADDR_LEN;
  IEEE80211_ADDR_COPY(aad, wh->i_addr2);
  aad += IEEE80211_ADDR_LEN;
  IEEE80211_ADDR_COPY(aad, wh->i_addr3);
  aad += IEEE80211_ADDR_LEN;
  *aad++ = wh->i_seq[0] & ~0xf0;
  *aad++ = 0;
  if (ieee80211_has_addr4(wh))
    {
      IEEE80211_ADDR_COPY(aad,
                          ((const struct ieee80211_frame_addr4 *)wh)->i_addr4);
      aad += IEEE80211_ADDR_LEN;
    }
  if (ieee80211_has_qos(wh))
    {
      *aad++ = tid = ieee80211_get_qos(wh) & IEEE80211_QOS_TID;
      *aad++ = 0;
    }

  /* construct CCM nonce */
  nonce[0] = tid;
  if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT)
    nonce[0] |= 1 << 4;         /* 11w: set management bit */
  IEEE80211_ADDR_COPY(&nonce[1], wh->i_addr2);
  nonce[7] = pn >> 40;          /* PN5 */
  nonce[8] = pn >> 32;          /* PN4 */
  nonce[9] = pn >> 24;          /* PN3 */
  nonce[10] = pn >> 16;         /* PN2 */
  nonce[11] = pn >> 8;          /* PN1 */
  nonce[12] = pn;               /* PN0 */

  /* add 2 authentication blocks (including l(a) and padded AAD) */
  la = aad - &auth[2];          /* fill l(a) */
  auth[0] = la >> 8;
  auth[1] = la & 0xff;
  memset(aad, 0, 30 - la);      /* pad AAD with zeros */

  /* construct first block B_0 */
  b[0] = 89;                    /* Flags = 64*Adata + 8*((M-2)/2) + (L-1) */
  memcpy(&b[1], nonce, 13);
  b[14] = lm >> 8;
  b[15] = lm & 0xff;
  rijndael_encrypt(ctx, b, b);

  for (i = 0; i < 16; i++)
    b[i] ^= auth[i];
  rijndael_encrypt(ctx, b, b);
  for (i = 0; i < 16; i++)
    b[i] ^= auth[16 + i];
  rijndael_encrypt(ctx, b, b);

  /* construct S_0 */
  a[0] = 1;                     /* Flags = L' = (L-1) */
  memcpy(&a[1], nonce, 13);
  a[14] = a[15] = 0;
  rijndael_encrypt(ctx, a, s0);
}

struct iob_s *ieee80211_ccmp_encrypt(struct ieee80211_s *ic, struct iob_s *iob0,
                                     struct ieee80211_key *k)
{
  struct ieee80211_ccmp_ctx *ctx = k->k_priv;
  const struct ieee80211_frame *wh;
  const uint8_t *src;
  uint8_t *ivp, *mic, *dst;
  uint8_t a[16], b[16], s0[16], s[16];
  struct iob_s *next0, *iob, *next;
  int hdrlen, left, moff, noff, len;
  uint16_t ctr;
  int i, j;

  next0 = iob_alloc(false);
  if (next0 == NULL)
    {
      goto nospace;
    }

  if (iob_clone(next0, iob0) < 0)
    {
      goto nospace;
    }

  next0->io_pktlen += IEEE80211_CCMP_HDRLEN;
  next0->io_len = CONFIG_IEEE80211_BUFSIZE;

  if (next0->io_len > next0->io_pktlen)
    {
      next0->io_len = next0->io_pktlen;
    }

  /* Copy 802.11 header */

  wh = (FAR struct ieee80211_frame *)IOB_DATA(iob0);
  hdrlen = ieee80211_get_hdrlen(wh);
  memcpy(IOB_DATA(next0), wh, hdrlen);

  k->k_tsc++;                   /* increment the 48-bit PN */

  /* construct CCMP header */

  ivp = (FAR uint8_t *) IOB_DATA(next0) + hdrlen;
  ivp[0] = k->k_tsc;            /* PN0 */
  ivp[1] = k->k_tsc >> 8;       /* PN1 */
  ivp[2] = 0;                   /* Rsvd */
  ivp[3] = k->k_id << 6 | IEEE80211_WEP_EXTIV;  /* KeyID | ExtIV */
  ivp[4] = k->k_tsc >> 16;      /* PN2 */
  ivp[5] = k->k_tsc >> 24;      /* PN3 */
  ivp[6] = k->k_tsc >> 32;      /* PN4 */
  ivp[7] = k->k_tsc >> 40;      /* PN5 */

  /* construct initial B, A and S_0 blocks */
  ieee80211_ccmp_phase1(&ctx->rijndael, wh, k->k_tsc,
                        iob0->io_pktlen - hdrlen, b, a, s0);

  /* construct S_1 */
  ctr = 1;
  a[14] = ctr >> 8;
  a[15] = ctr & 0xff;
  rijndael_encrypt(&ctx->rijndael, a, s);

  /* encrypt frame body and compute MIC */
  j = 0;
  iob = iob0;
  next = next0;
  moff = hdrlen;
  noff = hdrlen + IEEE80211_CCMP_HDRLEN;
  left = iob0->io_pktlen - moff;
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

      src = (FAR uint8_t *) IOB_DATA(iob) + moff;
      dst = (FAR uint8_t *) IOB_DATA(next) + noff;

      for (i = 0; i < len; i++)
        {
          /* update MIC with clear text */
          b[j] ^= src[i];

          /* encrypt message */
          dst[i] = src[i] ^ s[j];
          if (++j < 16)
            continue;

          /* we have a full block, encrypt MIC */
          rijndael_encrypt(&ctx->rijndael, b, b);

          /* construct a new S_ctr block */
          ctr++;
          a[14] = ctr >> 8;
          a[15] = ctr & 0xff;
          rijndael_encrypt(&ctx->rijndael, a, s);
          j = 0;
        }

      moff += len;
      noff += len;
      left -= len;
    }
  if (j != 0)                   /* partial block, encrypt MIC */
    rijndael_encrypt(&ctx->rijndael, b, b);

  /* Reserve trailing space for MIC */

  if (IOB_FREESPACE(next) < IEEE80211_CCMP_MICLEN)
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

  /* Finalize MIC, U := T XOR first-M-bytes( S_0 ) */

  mic = (FAR uint8_t *) IOB_DATA(next) + next->io_len;
  for (i = 0; i < IEEE80211_CCMP_MICLEN; i++)
    {
      mic[i] = b[i] ^ s0[i];
    }

  next->io_len += IEEE80211_CCMP_MICLEN;
  next0->io_pktlen += IEEE80211_CCMP_MICLEN;

  iob_free_chain(iob0);
  return next0;

nospace:
  iob_free_chain(iob0);
  if (next0 != NULL)
    {
      iob_free_chain(next0);
    }

  return NULL;
}

struct iob_s *ieee80211_ccmp_decrypt(struct ieee80211_s *ic, struct iob_s *iob0,
                                     struct ieee80211_key *k)
{
  struct ieee80211_ccmp_ctx *ctx = k->k_priv;
  struct ieee80211_frame *wh;
  uint64_t pn, *prsc;
  const uint8_t *ivp;
  const uint8_t *src;
  uint8_t *dst;
  uint8_t mic0[IEEE80211_CCMP_MICLEN];
  uint8_t a[16];
  uint8_t b[16];
  uint8_t s0[16];
  uint8_t s[16];
  struct iob_s *next0;
  struct iob_s *iob;
  struct iob_s *next;
  int hdrlen;
  int left;
  int moff;
  int noff;
  int len;
  uint16_t ctr;
  int i;
  int j;

  wh = (FAR struct ieee80211_frame *)IOB_DATA(iob0);
  hdrlen = ieee80211_get_hdrlen(wh);
  ivp = (uint8_t *) wh + hdrlen;

  if (iob0->io_pktlen < hdrlen + IEEE80211_CCMP_HDRLEN + IEEE80211_CCMP_MICLEN)
    {
      iob_free_chain(iob0);
      return NULL;
    }

  /* Check that ExtIV bit is set */

  if (!(ivp[3] & IEEE80211_WEP_EXTIV))
    {
      iob_free_chain(iob0);
      return NULL;
    }

  /* Retrieve last seen packet number for this frame type/priority */

  if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA)
    {
      uint8_t tid =
        ieee80211_has_qos(wh) ? ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;
      prsc = &k->k_rsc[tid];
    }
  else
    {
      /* 11w: management frames have their own counters */

      prsc = &k->k_mgmt_rsc;
    }

  /* Extract the 48-bit PN from the CCMP header */

  pn = (uint64_t) ivp[0] |
    (uint64_t) ivp[1] << 8 |
    (uint64_t) ivp[4] << 16 |
    (uint64_t) ivp[5] << 24 | (uint64_t) ivp[6] << 32 | (uint64_t) ivp[7] << 40;

  if (pn <= *prsc)
    {
      /* Replayed frame, discard */

      iob_free_chain(iob0);
      return NULL;
    }

  next0 = iob_alloc(false);
  if (next0 == NULL)
    {
      goto nospace;
    }

  if (iob_clone(next0, iob0) < 0)
    {
      goto nospace;
    }

  next0->io_pktlen -= IEEE80211_CCMP_HDRLEN + IEEE80211_CCMP_MICLEN;
  next0->io_len = CONFIG_IEEE80211_BUFSIZE;
  if (next0->io_len > next0->io_pktlen)
    {
      next0->io_len = next0->io_pktlen;
    }

  /* Construct initial B, A and S_0 blocks */

  ieee80211_ccmp_phase1(&ctx->rijndael, wh, pn,
                        next0->io_pktlen - hdrlen, b, a, s0);

  /* Copy 802.11 header and clear protected bit */

  memcpy(IOB_DATA(next0), wh, hdrlen);
  wh = (FAR struct ieee80211_frame *)IOB_DATA(next0);
  wh->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;

  /* construct S_1 */
  ctr = 1;
  a[14] = ctr >> 8;
  a[15] = ctr & 0xff;
  rijndael_encrypt(&ctx->rijndael, a, s);

  /* decrypt frame body and compute MIC */
  j = 0;
  iob = iob0;
  next = next0;
  moff = hdrlen + IEEE80211_CCMP_HDRLEN;
  noff = hdrlen;
  left = next0->io_pktlen - noff;
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

      src = (FAR uint8_t *) IOB_DATA(iob) + moff;
      dst = (FAR uint8_t *) IOB_DATA(next) + noff;

      for (i = 0; i < len; i++)
        {
          /* decrypt message */

          dst[i] = src[i] ^ s[j];

          /* update MIC with clear text */

          b[j] ^= dst[i];
          if (++j < 16)
            continue;
          /* we have a full block, encrypt MIC */

          rijndael_encrypt(&ctx->rijndael, b, b);

          /* construct a new S_ctr block */

          ctr++;
          a[14] = ctr >> 8;
          a[15] = ctr & 0xff;
          rijndael_encrypt(&ctx->rijndael, a, s);
          j = 0;
        }

      moff += len;
      noff += len;
      left -= len;
    }

  if (j != 0)
    {
      /* Partial block, encrypt MIC */

      rijndael_encrypt(&ctx->rijndael, b, b);
    }

  /* Finalize MIC, U := T XOR first-M-bytes( S_0 ) */

  for (i = 0; i < IEEE80211_CCMP_MICLEN; i++)
    b[i] ^= s0[i];

  /* Check that it matches the MIC in received frame */

  iob_copyout(mic0, iob, moff, IEEE80211_CCMP_MICLEN);
  if (memcmp(mic0, b, IEEE80211_CCMP_MICLEN) != 0)
    {
      iob_free_chain(iob0);
      iob_free_chain(next0);
      return NULL;
    }

  /* update last seen packet number (MIC is validated) */
  *prsc = pn;

  iob_free_chain(iob0);
  return next0;

nospace:
  iob_free_chain(iob0);
  if (next0 != NULL)
    {
      iob_free_chain(next0);
    }

  return NULL;
}
