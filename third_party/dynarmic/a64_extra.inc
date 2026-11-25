// SPDX-License-Identifier: 0BSD
// ARMv8.0 entries that a64.inc has commented out (dynarmic does not implement them), copied here
// unchanged and enabled so Biwa decodes every ARMv8.0 instruction (see UPSTREAM.md). Same format
// as a64.inc; src/a64/CMakeLists.txt appends them to the table.
INST(SQDMLAL_vec_1,          "SQDMLAL, SQDMLAL2 (vector)",                "01011110zz1mmmmm100100nnnnnddddd")
INST(SQDMLSL_vec_1,          "SQDMLSL, SQDMLSL2 (vector)",                "01011110zz1mmmmm101100nnnnnddddd")
INST(SQDMULL_vec_1,          "SQDMULL, SQDMULL2 (vector)",                "01011110zz1mmmmm110100nnnnnddddd")
INST(SQRSHL_1,               "SQRSHL",                                    "01011110zz1mmmmm010111nnnnnddddd")
INST(UQRSHL_1,               "UQRSHL",                                    "01111110zz1mmmmm010111nnnnnddddd")
INST(SQRSHRN_1,              "SQRSHRN, SQRSHRN2",                         "010111110IIIIiii100111nnnnnddddd")
INST(SQRSHRUN_1,             "SQRSHRUN, SQRSHRUN2",                       "011111110IIIIiii100011nnnnnddddd")
INST(UQRSHRN_1,              "UQRSHRN, UQRSHRN2",                         "011111110IIIIiii100111nnnnnddddd")
INST(SQDMLAL_elt_1,          "SQDMLAL, SQDMLAL2 (by element)",            "01011111zzLMmmmm0011H0nnnnnddddd")
INST(SQDMLSL_elt_1,          "SQDMLSL, SQDMLSL2 (by element)",            "01011111zzLMmmmm0111H0nnnnnddddd")
INST(SQDMLAL_vec_2,          "SQDMLAL, SQDMLAL2 (vector)",                "0Q001110zz1mmmmm100100nnnnnddddd")
INST(SQDMLSL_vec_2,          "SQDMLSL, SQDMLSL2 (vector)",                "0Q001110zz1mmmmm101100nnnnnddddd")
INST(SQRSHL_2,               "SQRSHL",                                    "0Q001110zz1mmmmm010111nnnnnddddd")
INST(UQRSHL_2,               "UQRSHL",                                    "0Q101110zz1mmmmm010111nnnnnddddd")
INST(SQDMLAL_elt_2,          "SQDMLAL, SQDMLAL2 (by element)",            "0Q001111zzLMmmmm0011H0nnnnnddddd")
INST(SQDMLSL_elt_2,          "SQDMLSL, SQDMLSL2 (by element)",            "0Q001111zzLMmmmm0111H0nnnnnddddd")
