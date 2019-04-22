#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <oblas.h>

#include "bitmask.h"
#include "chooser.h"
#include "graph.h"
#include "params.h"
#include "precode.h"
#include "rand.h"

static void precode_matrix_init_LDPC1(sparsemat *A, uint16_t S, uint16_t B) {
  int row, col;
  for (row = 0; row < S; row++) {
    for (col = 0; col < B; col++) {
      uint16_t submtx = col / S;
      if ((row == (col % S)) || (row == (col + submtx + 1) % S) ||
          (row == (col + 2 * (submtx + 1)) % S)) {
        sm_set(A, row, col, 1);
      }
    }
  }
}

static void precode_matrix_init_LDPC2(sparsemat *A, uint16_t skip,
                                      uint16_t rows, uint16_t cols) {
  for (int row = 0; row < rows; row++) {
    uint16_t start = row % cols;
    for (int col = 0; col < cols; col++) {
      uint8_t val = (col == start || col == (start + 1) % cols) > 0;
      if (val > 0) {
        sm_set(A, row, skip + col, val);
      }
    }
  }
}

static void precode_matrix_add_identity(sparsemat *A, uint16_t size,
                                        uint16_t skip_row, uint16_t skip_col) {
  for (int diag = 0; diag < size; diag++) {
    sm_set(A, skip_row + diag, skip_col + diag, 1);
  }
}

static sparsemat precode_matrix_make_MT(uint16_t rows, uint16_t cols) {
  sparsemat MT = sm_new(rows, cols);

  for (int row = 0; row < MT.rows; row++) {
    int col;
    for (col = 0; col < MT.cols - 1; col++) {
      uint32_t tmp = rnd_get(col + 1, 6, MT.rows);
      if ((row == tmp) ||
          (row == (tmp + rnd_get(col + 1, 7, MT.rows - 1) + 1) % MT.rows)) {
        sm_set(&MT, row, col, 1);
      }
    }
    sm_set(&MT, row, col, OCT_EXP[row]);
  }
  return MT;
}

static sparsemat precode_matrix_make_GAMMA(uint16_t dim) {
  sparsemat GAMMA = sm_new(dim, dim);

  for (int row = 0; row < GAMMA.rows; row++) {
    int col;
    for (col = 0; col <= row; col++) {
      sm_set(&GAMMA, row, col, OCT_EXP[(row - col) % OCT_EXP_SIZE]);
    }
  }
  return GAMMA;
}

static void precode_matrix_init_HDPC(sparsemat *A, struct pparams *prm) {
  uint16_t m = prm->H;
  uint16_t n = prm->K_padded + prm->S;

  if (m == 0 || n == 0)
    return;

  sparsemat MT = precode_matrix_make_MT(m, n);
  sparsemat GAMMA = precode_matrix_make_GAMMA(n);
  sparsemat MTxGAMMA = sm_new(MT.rows, GAMMA.cols);

  sm_gemm(&MT, &GAMMA, &MTxGAMMA);

  int row, col;
  for (col = 0; col < GAMMA.cols; col++) {
    for (row = 0; row < MT.rows; row++) {
      uint8_t val = sm_get(&MTxGAMMA, row, col);
      if (val != 0) {
        sm_set(A, prm->S + row, col, val);
      }
    }
  }
  sm_destroy(&MT);
  sm_destroy(&GAMMA);
  sm_destroy(&MTxGAMMA);
}

static void precode_matrix_add_G_ENC(sparsemat *A, struct pparams *prm) {
  for (int row = prm->S + prm->H; row < prm->L; row++) {
    uint32_t isi = (row - prm->S) - prm->H;
    uint16_vec idxs = params_get_idxs(prm, isi);
    for (int idx = 0; idx < kv_size(idxs); idx++) {
      sm_set(A, row, kv_A(idxs, idx), 1);
    }
    kv_destroy(idxs);
  }
}

static void decode_phase0(struct pparams *prm, octmat *A, struct bitmask *mask,
                          repair_vec *repair_bin, uint16_t num_symbols,
                          uint16_t overhead) {

  size_t padding = prm->K_padded - num_symbols;
  uint16_t num_gaps = bitmask_gaps(mask, num_symbols);
  uint16_t rep_idx = 0;
  for (int gap = 0; gap < prm->L && num_gaps > 0; gap++) {
    if (bitmask_check(mask, gap))
      continue;
    uint16_t row = gap + prm->H + prm->S;
    for (int col = 0; col < A->cols; col++) {
      om_A(*A, row, col) = 0;
    }

    uint16_vec idxs =
        params_get_idxs(prm, kv_A(*repair_bin, rep_idx++).esi + padding);
    for (int idx = 0; idx < kv_size(idxs); idx++) {
      om_A(*A, row, kv_A(idxs, idx)) = 1;
    }
    kv_destroy(idxs);
    num_gaps--;
  }

  int rep_row = (uint16_t)(A->rows - overhead);
  for (; rep_row < A->rows; rep_row++) {
    for (int col = 0; col < A->cols; col++) {
      om_A(*A, rep_row, col) = 0;
    }
    uint16_vec idxs =
        params_get_idxs(prm, kv_A(*repair_bin, rep_idx++).esi + padding);
    for (int idx = 0; idx < kv_size(idxs); idx++) {
      om_A(*A, rep_row, kv_A(idxs, idx)) = 1;
    }
    kv_destroy(idxs);
  }
}

static bool decode_phase1(struct pparams *prm, octmat *A, octmat *X, octmat *D,
                          uint16_vec c, uint16_t *i_val, uint16_t *u_val) {
  uint16_t i = 0;
  uint16_t u = prm->P;

  struct chooser ch = chooser_init(A->rows);

  for (int row = 0; row < A->rows; row++) {
    bool is_hdpc = (row >= prm->S && row < (prm->S + prm->H));
    size_t row_degree = 0;
    for (int col = 0; col < A->cols - u; col++) {
      row_degree += (uint8_t)(om_A(*A, row, col));
    }
    chooser_add_tracking_pair(&ch, is_hdpc, row_degree);
  }

  while (i + u < prm->L) {
    uint16_t sub_rows = A->rows - i;
    uint16_t sub_cols = A->cols - i - u;
    uint16_t chosen, non_zero;
    struct graph *G = graph_new(sub_cols);

    non_zero = chooser_non_zero(&ch, A, G, i, sub_rows, sub_cols);
    if (non_zero == sub_cols + 1) {
      chooser_clear(&ch);
      graph_free(G);

      *i_val = 0;
      *u_val = 0;
      return false;
    }
    chosen = chooser_pick(&ch, G, i, sub_rows, non_zero);
    if (chosen != 0) {
      oswaprow(om_P(*A), i, chosen + i, A->cols);
      oswaprow(om_P(*X), i, chosen + i, X->cols);
      oswaprow(om_P(*D), i, chosen + i, D->cols);

      kv_swap(struct tracking_pair, ch.tracking, i, chosen + i);
    }

    if (om_A(*A, i, i) == 0) {
      int idx = 1;
      for (; idx < sub_cols; idx++) {
        if (om_A(*A, i, idx + i) != 0)
          break;
      }

      oswapcol(om_P(*A), i, i + idx, A->rows, A->cols);
      oswapcol(om_P(*X), i, i + idx, X->rows, X->cols);

      kv_swap(uint16_t, c, i, i + idx);
    }
    int col = sub_cols - 1;
    int swap = 1;
    for (; col > sub_cols - non_zero; col--) {
      if (om_A(*A, i, col + i) != 0)
        continue;
      while (swap < col && om_A(*A, i, swap + i) == 0) {
        swap++;
      }

      if (swap >= col)
        break;

      oswapcol(om_P(*A), col + i, swap + i, A->rows, A->cols);
      oswapcol(om_P(*X), col + i, swap + i, X->rows, X->cols);

      kv_swap(uint16_t, c, col + i, swap + i);
    }

    for (int row = 1; row < sub_rows; row++) {
      if (om_A(*A, row + i, i) != 0) {
        uint8_t mnum = om_A(*A, row + i, i);
        uint8_t mden = om_A(*A, i, i);
        uint8_t multiple = (mnum > 0 && mden > 0) ? OCTET_DIV(mnum, mden) : 0;
        if (multiple == 0)
          continue;
        oaxpy(om_P(*A), om_P(*A), row + i, i, A->cols, multiple);
        oaxpy(om_P(*D), om_P(*D), row + i, i, D->cols, multiple);
      }
    }
    i++;
    u += non_zero - 1;

    graph_free(G);
  }
  chooser_clear(&ch);

  *i_val = i;
  *u_val = u;
  return true;
}

static bool decode_phase2(octmat *A, octmat *D, uint16_t i, uint16_t u,
                          uint16_t L) {

  uint16_t row_start = i, row_end = A->rows;
  uint16_t col_start = A->cols - u;

  for (int row = row_start; row < row_end; row++) {
    int row_nonzero = row;
    int diag = col_start + (row - row_start);
    if (diag >= L) {
      break;
    }
    for (; row_nonzero < row_end; row_nonzero++) {
      if (om_A(*A, row_nonzero, diag) != 0) {
        break;
      }
    }
    if (row != row_nonzero) {
      oswaprow(om_P(*A), row, row_nonzero, A->cols);
      oswaprow(om_P(*D), row, row_nonzero, D->cols);
    }

    if (om_A(*A, row, diag) > 1) {
      uint8_t multiple = om_A(*A, row, diag);
      oscal(om_P(*A), row, A->cols, OCTET_DIV(1, multiple));
      oscal(om_P(*D), row, D->cols, OCTET_DIV(1, multiple));
    }

    for (int del_row = row_start; del_row < row_end; del_row++) {
      if (del_row == row)
        continue;
      uint8_t multiple = om_A(*A, del_row, diag);
      if (multiple == 0)
        continue;
      oaxpy(om_P(*A), om_P(*A), del_row, row, A->cols, multiple);
      oaxpy(om_P(*D), om_P(*D), del_row, row, D->cols, multiple);
    }
  }
  return true;
}

static void decode_phase3(octmat *A, octmat *X, octmat *D, uint16_t i) {
  octmat Xb = OM_INITIAL;
  octmat Ab = OM_INITIAL;
  octmat Db = OM_INITIAL;

  om_resize(&Xb, i, i);
  for (int row = 0; row < i; row++) {
    for (int col = 0; col < i; col++) {
      om_A(Xb, row, col) = om_A(*X, row, col);
    }
  }

  om_copy(&Ab, A);
  om_copy(&Db, D);
  ogemm(om_P(Xb), om_P(Ab), om_P(*A), i, i, Ab.cols);
  ogemm(om_P(Xb), om_P(Db), om_P(*D), i, i, Db.cols);
  om_destroy(&Ab);
  om_destroy(&Xb);
  om_destroy(&Db);
}

static void decode_phase4(octmat *A, octmat *D, uint16_t i, uint16_t u) {
  uint16_t skip = A->cols - u;

  for (int row = 0; row < i; row++) {
    for (int col = 0; col < u; col++) {
      uint8_t multiple = om_A(*A, row, col + skip);
      if (multiple == 0)
        continue;
      oaxpy(om_P(*D), om_P(*D), row, i + col, D->cols, multiple);
    }
  }
}

static void decode_phase5(octmat *A, octmat *D, uint16_t i) {
  uint8_t multiple = 0;
  for (int j = 0; j <= i; j++) {
    if (om_A(*A, j, j) != 1) {
      multiple = om_A(*A, j, j);
      // oscal(om_P(*A), j, A->cols, OCTET_DIV(1, multiple));
      oscal(om_P(*D), j, D->cols, OCTET_DIV(1, multiple));
    }
    for (int l = 0; l < j; l++) {
      multiple = om_A(*A, j, l);
      if (multiple == 0)
        continue;
      oaxpy(om_P(*A), om_P(*A), j, l, A->cols, multiple);
      oaxpy(om_P(*D), om_P(*D), j, l, D->cols, multiple);
    }
  }
}

void precode_matrix_gen(struct pparams *prm, sparsemat *A, uint16_t overhead) {
  precode_matrix_init_LDPC1(A, prm->S, prm->B);
  precode_matrix_add_identity(A, prm->S, 0, prm->B);
  precode_matrix_init_LDPC2(A, prm->W, prm->S, prm->P);
  precode_matrix_init_HDPC(A, prm);
  precode_matrix_add_identity(A, prm->H, prm->S, prm->L - prm->H);
  precode_matrix_add_G_ENC(A, prm);
}

octmat precode_matrix_intermediate1(struct pparams *prm, octmat *A, octmat *D) {
  bool success;
  uint16_t i, u;

  uint16_vec c;
  kv_init(c);

  octmat C = OM_INITIAL;
  octmat X = OM_INITIAL;

  if (prm->L == 0 || A == NULL || A->rows == 0 || A->cols == 0) {
    return C;
  }

  om_copy(&X, A);
  kv_resize(uint16_t, c, prm->L);
  for (int l = 0; l < prm->L; l++) {
    kv_push(uint16_t, c, l);
  }

  success = decode_phase1(prm, A, &X, D, c, &i, &u);
  if (!success) {
    kv_destroy(c);
    om_destroy(&X);
    return C;
  }

  success = decode_phase2(A, D, i, u, prm->L);
  if (!success) {
    kv_destroy(c);
    om_destroy(&X);
    return C;
  }

  decode_phase3(A, &X, D, i);
  om_destroy(&X);
  decode_phase4(A, D, i, u);
  decode_phase5(A, D, i);
  om_destroy(A);

  om_resize(&C, D->rows, D->cols);
  for (int l = 0; l < prm->L; l++) {
    ocopy(om_P(C), om_P(*D), kv_A(c, l), l, C.cols);
  }
  kv_destroy(c);

  return C;
}

bool precode_matrix_intermediate2(octmat *M, octmat *A, octmat *D,
                                  struct pparams *prm, repair_vec *repair_bin,
                                  struct bitmask *mask, uint16_t num_symbols,
                                  uint16_t overhead) {

  int num_gaps, gap = 0, row = 0;
  octmat C;

  if (D->cols == 0) {
    return false;
  }

  decode_phase0(prm, A, mask, repair_bin, num_symbols, overhead);

  C = precode_matrix_intermediate1(prm, A, D);
  if (C.rows == 0) {
    return false;
  }

  num_gaps = bitmask_gaps(mask, num_symbols);
  om_resize(M, num_gaps, D->cols);
  for (gap = 0; gap < num_symbols && num_gaps > 0; gap++) {
    if (bitmask_check(mask, gap))
      continue;
    octmat ret = precode_matrix_encode(prm, &C, gap);
    ocopy(om_P(*M), om_P(ret), row, 0, M->cols);
    om_destroy(&ret);
    row++;
    num_gaps--;
  }
  om_destroy(&C);
  return true;
}

octmat precode_matrix_encode(struct pparams *prm, octmat *C, uint32_t isi) {
  octmat ret = OM_INITIAL;
  om_resize(&ret, 1, C->cols);

  uint16_vec idxs = params_get_idxs(prm, isi);
  for (int idx = 0; idx < kv_size(idxs); idx++) {
    oaddrow(om_P(ret), om_P(*C), 0, kv_A(idxs, idx), ret.cols);
  }
  kv_destroy(idxs);

  return ret;
}

bool precode_matrix_decode(struct pparams *prm, octmat *X,
                           repair_vec *repair_bin, struct bitmask *mask) {
  uint16_t num_symbols = X->rows, rep_idx, num_gaps, num_repair, overhead;

  octmat A = OM_INITIAL;
  octmat D = OM_INITIAL;
  octmat M = OM_INITIAL;
  sparsemat As;

  num_repair = kv_size(*repair_bin);
  num_gaps = bitmask_gaps(mask, num_symbols);

  if (num_gaps == 0)
    return true;

  if (num_repair < num_gaps)
    return false;

  overhead = num_repair - num_gaps;
  rep_idx = 0;
  om_resize(&A, prm->L + overhead, prm->L);
  As = sm_new(prm->L + overhead, prm->L);
  precode_matrix_gen(prm, &As, overhead);
  // FIXME: WIP convert back to dense
  sm_densify(&As, om_P(A));
  sm_destroy(&As);

  om_resize(&D, prm->S + prm->H + prm->K_padded + overhead, X->cols);

  int skip = prm->S + prm->H;
  for (int row = 0; row < X->rows; row++) {
    ocopy(om_P(D), om_P(*X), skip + row, row, D.cols);
  }

  for (int gap = 0; gap < num_symbols && rep_idx < num_repair; gap++) {
    if (bitmask_check(mask, gap))
      continue;
    uint16_t row = skip + gap;
    struct repair_sym rs = kv_A(*repair_bin, rep_idx++);
    ocopy(om_P(D), om_P(rs.row), row, 0, D.cols);
  }

  for (int row = skip + prm->K_padded; rep_idx < num_repair; row++) {
    struct repair_sym rs = kv_A(*repair_bin, rep_idx++);
    ocopy(om_P(D), om_P(rs.row), row, 0, D.cols);
  }

  bool precode_ok = precode_matrix_intermediate2(&M, &A, &D, prm, repair_bin,
                                                 mask, num_symbols, overhead);
  om_destroy(&A);
  om_destroy(&D);

  if (!precode_ok)
    return false;

  int miss_row = 0;
  for (int row = 0; row < num_symbols && miss_row < M.rows; row++) {
    if (bitmask_check(mask, row))
      continue;
    miss_row++;
    if (bitmask_check(mask, row))
      continue;

    ocopy(om_P(*X), om_P(M), row, miss_row - 1, X->cols);
    bitmask_set(mask, row);
  }
  om_destroy(&M);
  return true;
}
