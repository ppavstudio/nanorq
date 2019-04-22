#ifndef NANORQ_PRECODE_H
#define NANORQ_PRECODE_H

#include "bitmask.h"
#include "params.h"

void precode_matrix_gen(struct pparams *prm, sparsemat *A, uint16_t overhead);

octmat precode_matrix_intermediate1(struct pparams *prm, sparsemat *A, octmat *D);
bool precode_matrix_intermediate2(octmat *M, sparsemat *A, octmat *D,
                                  struct pparams *prm, repair_vec *repair_bin,
                                  struct bitmask *mask, uint16_t num_symbols,
                                  uint16_t overhead);

octmat precode_matrix_encode(struct pparams *prm, octmat *C, uint32_t isi);

bool precode_matrix_decode(struct pparams *prm, octmat *X,
                           repair_vec *repair_bin, struct bitmask *mask);

#endif
