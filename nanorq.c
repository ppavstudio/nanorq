#include <stdio.h>

#include "nanorq.h"
#include "precode.h"

struct oti_common {
  size_t F;    /* input size in bytes */
  uint16_t T;  /* the symbol size in octets, which MUST be a multiple of Al */
  uint16_t Al; /* byte alignment, 0 < Al <= 8, 4 is recommended */
  uint16_t SS; /* sub symbol size (multiple of alignment) */
  size_t WS;   /* max sub block size decode */
};

struct oti_scheme {
  uint16_t Z; /* number of source blocks */
  uint32_t N; /* number of sub-blocks in each source block */
  size_t Kt;  /* the total number of symbols required to represent input */
};

struct partition {
  uint16_t IL; /* size of long blocks */
  uint16_t IS; /* size of short blocks*/
  uint16_t JL; /* number of long blocks */
  uint16_t JS; /* number of short blocks */
};

struct source_block {
  size_t sbloc;
  size_t part_tot;
  struct partition part;
  uint16_t al;
};

struct encoder_core {
  uint8_t sbn;
  uint16_t num_symbols;
  uint16_t symbol_size;
  struct pparams prm;
  octmat symbolmat;
};

struct decoder_core {
  uint8_t sbn;
  uint16_t num_symbols;
  uint16_t symbol_size;
  struct pparams prm;
  octmat symbolmat;
  repair_vec repair_bin;
  struct bitmask *mask;
};

struct nanorq {
  struct oti_common common;
  struct oti_scheme scheme;

  struct partition src_part; /* (KL, KS, ZL, ZS) = Partition[Kt, Z] */
  struct partition sub_part; /* (TL, TS, NL, NS) = Partition[T/Al, N] */

  struct encoder_core *encoders[UINT8_MAX];
  struct decoder_core *decoders[UINT8_MAX];
};

static struct oti_scheme gen_scheme_specific(struct oti_common *common) {
  struct oti_scheme ret = {0};
  size_t N_max = (size_t)(div_floor(common->T, common->SS)), n;
  ret.Kt = div_ceil(common->F, common->T);

  uint16_vec KL;
  kv_init(KL);
  kv_resize(uint16_t, KL, N_max);
  memset(KL.a, 0, kv_size(KL));

  for (n = 1; n <= N_max; n++) {
    size_t KL_max =
        (size_t)common->WS /
        ((size_t)common->Al * div_ceil(common->T, (size_t)(common->Al * n)));
    if (KL_max > UINT16_MAX)
      KL_max = K_max;
    int idx;
    for (idx = 0; idx < K_padded_size; idx++) {
      if (K_padded[idx] > KL_max)
        break;
    }
    kv_push(uint16_t, KL, K_padded[idx == 0 ? 0 : (idx - 1)]);
  }

  size_t Z_tmp = (size_t)(div_ceil(ret.Kt, kv_A(KL, N_max - 1)));
  if (Z_tmp > (UINT8_MAX + 1)) {
    return ret;
  }

  ret.Z = Z_tmp;
  uint16_t tmp = (uint16_t)(div_ceil(ret.Kt, ret.Z));
  for (n = 0; n < kv_size(KL); n++) {
    if (tmp <= kv_A(KL, n)) {
      ret.N = (uint16_t)(n + 1);
      break;
    }
  }
  kv_destroy(KL);

  return ret;
}

static struct partition fill_partition(size_t I, uint16_t J) {
  struct partition p = {0, 0, 0, 0};
  if (J == 0)
    return p;
  p.IL = (uint16_t)(div_ceil(I, J));
  p.IS = (uint16_t)(div_floor(I, J));
  p.JL = (uint16_t)(I - p.IS * J);
  p.JS = J - p.JL;

  if (p.JL == 0)
    p.IL = 0;
  return p;
}

static struct source_block get_source_block(nanorq *rq, uint8_t sbn,
                                            uint16_t symbol_size) {
  struct source_block ret;
  ret.part = rq->sub_part;
  ret.al = rq->common.Al;
  ret.sbloc = 0;
  ret.part_tot = rq->sub_part.IL * rq->sub_part.JL;

  if (sbn < rq->src_part.JL) {
    ret.sbloc = sbn * rq->src_part.IL * symbol_size;
  } else if (sbn - rq->src_part.JL < rq->src_part.JS) {
    ret.sbloc = (rq->src_part.IL * rq->src_part.JL) * symbol_size +
                (sbn - rq->src_part.JL) * rq->src_part.IS * symbol_size;
  }

  return ret;
}

static size_t get_symbol_offset(struct source_block *blk, size_t pos,
                                uint16_t K, uint32_t symbol_id) {
  size_t i;

  if (pos < blk->part_tot) {
    size_t sub_blk_id = pos / blk->part.IL;
    i = blk->sbloc + sub_blk_id * K * blk->part.IL + symbol_id * blk->part.IL +
        pos % blk->part.IL;
  } else {
    size_t pos_part2 = pos - blk->part_tot;
    size_t sub_blk_id = pos_part2 / blk->part.IS;
    i = blk->sbloc + (blk->part_tot * K) + sub_blk_id * K * blk->part.IS +
        symbol_id * blk->part.IS + pos_part2 % blk->part.IS;
  }

  return i * blk->al;
}

static struct encoder_core *nanorq_block_encoder(nanorq *rq, uint8_t sbn) {
  uint16_t num_symbols = nanorq_block_symbols(rq, sbn);
  uint16_t symbol_size = rq->common.T / rq->common.Al;

  if (rq->encoders[sbn])
    return rq->encoders[sbn];

  if (num_symbols == 0 || symbol_size == 0)
    return NULL;

  struct encoder_core *enc = calloc(1, sizeof(struct encoder_core));
  enc->sbn = sbn;
  enc->num_symbols = num_symbols;
  enc->symbol_size = symbol_size;
  enc->prm = params_init(num_symbols);
  rq->encoders[sbn] = enc;
  return enc;
}

bool nanorq_generate_symbols(nanorq *rq, uint8_t sbn, struct ioctx *io) {
  octmat A = OM_INITIAL, D = OM_INITIAL;
  sparsemat As;

  struct encoder_core *enc = nanorq_block_encoder(rq, sbn);
  struct pparams *prm = NULL;

  if (enc == NULL)
    return false;

  if (enc->symbolmat.rows > 0)
    return true;

  prm = &enc->prm;
  om_resize(&A, prm->L, prm->L);
  As = sm_new(prm->L, prm->L);

  precode_matrix_gen(prm, &As, 0);
  // FIXME: WIP convert back to dense
  sm_densify(&As, om_P(A), A.cols_al);
  sm_destroy(&As);

  om_resize(&D, prm->K_padded + prm->S + prm->H,
            enc->symbol_size * rq->common.Al);

  int row = 0, col = 0;
  for (row = 0; row < prm->S + prm->H; row++) {
    for (col = 0; col < D.cols; col++) {
      om_A(D, row, col) = 0;
    }
  }

  struct source_block blk = get_source_block(rq, sbn, enc->symbol_size);
  for (; row < prm->S + prm->H + enc->num_symbols; row++) {
    uint32_t symbol_id = row - (prm->S + prm->H);
    col = 0;
    for (int i = 0; i < enc->symbol_size;) {
      size_t offset = get_symbol_offset(&blk, i, enc->num_symbols, symbol_id);
      uint16_t sublen = (i < blk.part_tot) ? blk.part.IL : blk.part.IS;
      uint16_t stride = sublen * rq->common.Al;
      uint8_t buf[stride];
      i += sublen;

      size_t got = 0;
      if (io->seek(io, offset)) {
        got = io->read(io, buf, stride);
      }
      for (int byte = 0; byte < got; byte++) {
        om_A(D, row, col++) = buf[byte];
      }
      for (int byte = got; byte < stride; byte++) {
        om_A(D, row, col++) = 0;
      }
    }
  }

  for (; row < D.rows; row++) {
    for (int col = 0; col < D.cols; col++)
      om_A(D, row, col) = 0;
  }

  enc->symbolmat = precode_matrix_intermediate1(prm, &A, &D);
  if (enc->symbolmat.rows == 0)
    return false;

  om_destroy(&A);
  om_destroy(&D);

  return true;
}

nanorq *nanorq_encoder_new(uint64_t len, uint16_t T, uint16_t SS, uint8_t Al,
                           size_t WS) {
  nanorq *rq = NULL;

  if (T == 0 || Al == 0 || T < Al || T % Al != 0 || SS < Al || (SS % Al) != 0 ||
      SS > T) {
    return NULL;
  }

  rq = calloc(1, sizeof(nanorq));
  rq->common.F = len;
  rq->common.T = T;
  rq->common.Al = Al;
  rq->common.SS = SS;
  rq->common.WS = WS;

  rq->scheme = gen_scheme_specific(&rq->common);

  if (rq->scheme.Z == 0 || rq->scheme.N == 0 ||
      div_ceil(rq->scheme.Kt, rq->scheme.Z) > K_max) {
    free(rq);
    return NULL;
  }

  rq->src_part = fill_partition(rq->scheme.Kt, rq->scheme.Z);
  rq->sub_part = fill_partition(rq->common.T / rq->common.Al, rq->scheme.N);

#ifdef NANORQ_DEBUG
  fprintf(stderr, "T: %06d SS: %06d AL: %d WS: %06lu\n", T, SS, Al, WS);
  fprintf(stderr, "Z: %06d N : %06d       Kt: %06lu\n", rq->scheme.Z,
          rq->scheme.N, rq->scheme.Kt);

  fprintf(stderr, "P1 %dx%d P2 %dx%d\n", rq->src_part.JL, rq->src_part.IL,
          rq->src_part.JS, rq->src_part.IS);
#endif

  return rq;
}

void nanorq_free(nanorq *rq) {
  int num_sbn = nanorq_blocks(rq);
  if (rq) {
    for (int sbn = 0; sbn < num_sbn; sbn++) {
      nanorq_encode_cleanup(rq, sbn);
      nanorq_decode_cleanup(rq, sbn);
    }
    free(rq);
  }
}

uint64_t nanorq_oti_common(nanorq *rq) {
  uint64_t ret = 0;
  ret = ((uint64_t)rq->common.F) << 24; /* transfer length */
  ret |= rq->common.T;                  /* symbol size */

  return ret;
}

uint32_t nanorq_oti_scheme_specific(nanorq *rq) {
  uint32_t ret = 0;
  ret = (rq->scheme.Z % (UINT8_MAX + 1)) << 24;  /* number of source blocks */
  ret |= (rq->scheme.N % (UINT16_MAX + 1)) << 8; /* number of sub-blocks */
  ret |= rq->common.Al;                          /* symbol alignment */

  return ret;
}

uint32_t nanorq_fid(uint8_t sbn, uint32_t esi) {
  uint32_t ret = (uint32_t)(sbn) << 24;
  ret += esi % (uint32_t)(1 << 24);
  return ret;
}

uint64_t nanorq_transfer_length(nanorq *rq) { return rq->common.F; }

uint16_t nanorq_symbol_size(nanorq *rq) { return rq->common.T; }

nanorq *nanorq_decoder_new(uint64_t common, uint32_t scheme) {
  uint64_t F = common >> 24;
  uint16_t T = common & 0xffff;

  nanorq *rq = NULL;

  if (F > NANORQ_MAX_TRANSFER)
    return NULL;

  rq = calloc(1, sizeof(nanorq));

  rq->common.F = F;
  rq->common.T = T;

  rq->scheme.Z = (scheme >> 24) & 0xff;
  rq->scheme.N = (scheme >> 8) & 0xffff;
  rq->common.Al = scheme & 0xff;
  rq->scheme.Kt = div_ceil(rq->common.F, rq->common.T);

  if (rq->scheme.Z == 0)
    rq->scheme.Z = (UINT8_MAX + 1);
  if (rq->scheme.N == 0)
    rq->scheme.N = (UINT16_MAX + 1);

  if (rq->common.T < rq->common.Al || rq->common.T % rq->common.Al != 0 ||
      div_ceil(div_ceil(rq->common.F, rq->common.T), rq->scheme.Z) > K_max) {
    free(rq);
    return NULL;
  }

  rq->src_part = fill_partition(rq->scheme.Kt, rq->scheme.Z);
  rq->sub_part = fill_partition(rq->common.T / rq->common.Al, rq->scheme.N);

#ifdef NANORQ_DEBUG
  fprintf(stderr, "T: %06d SS: XXXXXX AL: %d WS: XXXXXX\n", rq->common.T,
          rq->common.Al);
  fprintf(stderr, "Z: %06d N : %06d       Kt: %06lu\n", rq->scheme.Z,
          rq->scheme.N, rq->scheme.Kt);

  fprintf(stderr, "P1 %dx%d P2 %dx%d\n", rq->src_part.JL, rq->src_part.IL,
          rq->src_part.JS, rq->src_part.IS);
#endif

  return rq;
}

/*
+   num(0) JL
+   num(1) JS
+   size(0) IL
+   size(1) IS
*/

uint16_t nanorq_block_symbols(nanorq *rq, uint8_t sbn) {
  if (sbn < rq->src_part.JL)
    return rq->src_part.IL;
  if (sbn - rq->src_part.JL < rq->src_part.JS)
    return rq->src_part.IS;
  return 0;
}

uint32_t nanorq_encoder_max_repair(nanorq *rq, uint8_t sbn) {
  return (uint32_t)((1 << 20) - nanorq_block_symbols(rq, sbn));
}

uint8_t nanorq_blocks(nanorq *rq) {
  return (uint8_t)(rq->src_part.JL + rq->src_part.JS);
}

uint64_t nanorq_encode(nanorq *rq, void *data, uint32_t esi, uint8_t sbn,
                       struct ioctx *io) {
  uint64_t written = 0;

  struct encoder_core *enc = nanorq_block_encoder(rq, sbn);
  if (enc == NULL)
    return 0;

  if (esi < enc->num_symbols) {
    struct source_block blk = get_source_block(rq, sbn, enc->symbol_size);
    uint8_t *dst = ((uint8_t *)data);
    for (int i = 0; i < enc->symbol_size;) {
      size_t offset = get_symbol_offset(&blk, i, enc->num_symbols, esi);
      uint16_t sublen = (i < blk.part_tot) ? blk.part.IL : blk.part.IS;
      uint16_t stride = sublen * rq->common.Al;
      uint8_t buf[stride];
      i += sublen;

      int got = 0;
      if (io->seek(io, offset)) {
        got = io->read(io, buf, stride);
      }
      for (int byte = 0; byte < got; byte++) {
        *dst = buf[byte];
        dst++;
        written++;
      }
      for (int byte = got; byte < stride; byte++) {
        *dst = 0;
        dst++;
        written++;
      }
    }
  } else {
    // esi is for repair symbol
    struct pparams *prm = &enc->prm;
    if (enc->symbolmat.rows == 0) {
      bool generated = nanorq_generate_symbols(rq, sbn, io);
      if (!generated)
        return 0;
    }

    uint32_t isi = esi + (prm->K_padded - enc->num_symbols);
    octmat tmp = precode_matrix_encode(prm, &enc->symbolmat, isi);
    uint8_t *dst = ((uint8_t *)data);
    uint8_t *octet = om_P(tmp);
    for (int i = 0; i < enc->symbol_size; i++) {
      for (int byte = 0; byte < rq->common.Al; byte++) {
        *dst = (octet == NULL) ? 0 : *(octet++);
        dst++;
        written++;
      }
    }
    om_destroy(&tmp);
  }
  return written;
}

void nanorq_encode_cleanup(nanorq *rq, uint8_t sbn) {
  if (rq->encoders[sbn]) {
    struct encoder_core *enc = rq->encoders[sbn];
    om_destroy(&enc->symbolmat);
    free(enc);
    rq->encoders[sbn] = NULL;
  }
}

static struct decoder_core *nanorq_block_decoder(nanorq *rq, uint8_t sbn) {
  uint16_t num_symbols = nanorq_block_symbols(rq, sbn);
  uint16_t symbol_size = rq->common.T / rq->common.Al;

  if (rq->decoders[sbn])
    return rq->decoders[sbn];

  if (num_symbols == 0 || symbol_size == 0)
    return NULL;

  struct decoder_core *dec = calloc(1, sizeof(struct decoder_core));
  dec->sbn = sbn;
  dec->num_symbols = num_symbols;
  dec->symbol_size = symbol_size;
  dec->prm = params_init(num_symbols);
  dec->mask = bitmask_new(num_symbols);
  om_resize(&dec->symbolmat, num_symbols, symbol_size * rq->common.Al);

  rq->decoders[sbn] = dec;
  return dec;
}

bool nanorq_decoder_add_symbol(nanorq *rq, void *data, uint32_t fid) {

  uint8_t sbn = fid >> 24;
  uint32_t esi = (fid & 0x00ffffff);

  struct decoder_core *dec = nanorq_block_decoder(rq, sbn);

  if (dec == NULL)
    return false;

  uint16_t cols = dec->symbolmat.cols;

  if (esi >= (1 << 20))
    return false;

  if (bitmask_gaps(dec->mask, dec->num_symbols) == 0) {
    return true; // no gaps! no repair needed.
  }

  if (bitmask_check(dec->mask, esi))
    return true; // already got this esi

  if (esi < dec->num_symbols) {
    memcpy(om_R(dec->symbolmat, esi), data, cols);
  } else {
    struct repair_sym rs = {esi, OM_INITIAL};
    om_resize(&rs.row, 1, cols);
    memcpy(om_R(rs.row, 0), data, cols);
    kv_push(struct repair_sym, dec->repair_bin, rs);
  }
  bitmask_set(dec->mask, esi);

  return true;
}

uint32_t nanorq_num_missing(nanorq *rq, uint8_t sbn) {
  uint16_t num_symbols = nanorq_block_symbols(rq, sbn);
  struct decoder_core *dec = nanorq_block_decoder(rq, sbn);
  if (dec == NULL)
    return 0;

  return bitmask_gaps(dec->mask, num_symbols);
}

uint32_t nanorq_num_repair(nanorq *rq, uint8_t sbn) {
  struct decoder_core *dec = nanorq_block_decoder(rq, sbn);
  if (dec == NULL)
    return 0;

  return kv_size(dec->repair_bin);
}

uint64_t nanorq_decode_block(nanorq *rq, struct ioctx *io, uint8_t sbn) {
  uint64_t written = 0;

  struct decoder_core *dec = nanorq_block_decoder(rq, sbn);
  struct pparams *prm = &dec->prm;
  if (dec == NULL)
    return 0;

  bool success =
      precode_matrix_decode(prm, &dec->symbolmat, &dec->repair_bin, dec->mask);
  if (!success) {
    return 0;
  }

  int max_esi = dec->symbolmat.rows;
  int row = 0, col = 0;
  struct source_block blk = get_source_block(rq, sbn, dec->symbol_size);
  for (; row < max_esi; row++) {
    col = 0;
    for (int i = 0; i < dec->symbol_size;) {
      size_t offset = get_symbol_offset(&blk, i, max_esi, row);
      uint16_t sublen = (i < blk.part_tot) ? blk.part.IL : blk.part.IS;
      uint16_t stride = sublen * rq->common.Al;
      i += sublen;

      if (io->seek(io, offset)) {
        uint16_t len = stride;
        if (offset >= rq->common.F)
          continue;
        if ((offset + stride) >= rq->common.F) {
          len = (rq->common.F - offset);
        }
        written += io->write(io, om_R(dec->symbolmat, row) + col, len);
        col += stride;
      }
    }
  }

  return written;
}

void nanorq_decode_cleanup(nanorq *rq, uint8_t sbn) {
  if (rq->decoders[sbn]) {
    struct decoder_core *dec = rq->decoders[sbn];
    om_destroy(&dec->symbolmat);
    if (kv_size(dec->repair_bin) > 0) {
      for (int rs = 0; rs < kv_size(dec->repair_bin); rs++) {
        om_destroy(&(kv_A(dec->repair_bin, rs).row));
      }
      kv_destroy(dec->repair_bin);
    }
    bitmask_free(dec->mask);
    free(dec);
    rq->decoders[sbn] = NULL;
  }
}
