/*
 * Code Copyright (c) 2017, Nicola Pierazzo <nicolapierazzo@gmail.com>,
 *                          Gabriele Facciolo <gfacciol@gmail.com>
 * Based on the 2010 article by Guoshen Yu <yu@cmap.polytechnique.fr>,
 *                              Guillermo Sapiro <guille@umn.edu>
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/*-----------------------  Multiscale DCTdenoising  -------------------------*/
// This code implements "Multiscale DCT denoising".
// http://www.ipol.im/pub/art/2017/201 
// Copyright, Nicola Pierazzo, Gabriele Facciolo, 2017.
// Please report bugs and/or send comments to G. Facciolo gfacciol@gmail.com
/*---------------------------------------------------------------------------*/


#include <cmath>
#include <utility>
#include <tuple>
#include <vector>

#include "Image.hpp"
#include "DCTPatch.hpp"
#include "utils.hpp"
#include "DCTdenoising.h"

#ifdef _OPENMP
#include <omp.h>
#endif

using imgutils::Image;
using imgutils::DCTPatch;
using imgutils::ComputeTiling;
using imgutils::SplitTiles;
using imgutils::MergeTiles;
using std::move;
using std::sqrt;
using std::pair;
using std::abs;
using std::vector;
using std::copy;

constexpr float HARD_THRESHOLD = 3.f;

Image ColorTransform(Image &&src) {
  Image img = move(src);
  if (img.channels() == 3) {
    for (int row = 0; row < img.rows(); ++row) {
      for (int col = 0; col < img.columns(); ++col) {
        float r, g, b;
        r = img.val(col, row, 0);
        g = img.val(col, row, 1);
        b = img.val(col, row, 2);
        img.val(col, row, 0) = (r + g + b) / sqrt(3.f);
        img.val(col, row, 1) = (r - b) / sqrt(2.f);
        img.val(col, row, 2) = (r - 2 * g + b) / sqrt(6.f);
      }
    }
  }
  return img;
}

Image ColorTransformInverse(Image &&src) {
  Image img = move(src);
  if (img.channels() == 3) {
    for (int row = 0; row < img.rows(); ++row) {
      for (int col = 0; col < img.columns(); ++col) {
        float y, u, v;
        y = img.val(col, row, 0);
        u = img.val(col, row, 1);
        v = img.val(col, row, 2);
        img.val(col, row, 0) = (sqrt(2.f) * y + sqrt(3.f) * u + v) / sqrt(6.f);
        img.val(col, row, 1) = (y - sqrt(2.f) * v) / sqrt(3.f);
        img.val(col, row, 2) = (sqrt(2.f) * y - sqrt(3.f) * u + v) / sqrt(6.f);
      }
    }
  }
  return img;
}

void ExtractPatch(const Image &src, int pr, int pc, DCTPatch *dst) {
  // src is padded, so (pr, pc) becomes the upper left pixel
  for (int chan = 0; chan < dst->channels(); ++chan) {
    for (int row = 0; row < dst->rows(); ++row) {
//      for (int col = 0; col < dst->columns(); ++col) {
//        dst->space(col, row, chan) = src.val(pc + col, pr + row, chan);
//      }
      copy(&(src.val(pc, pr + row, chan)),
           &src.val(pc + dst->columns(), pr + row, chan),
           &dst->space(0, row, chan));
    }
  }
}

pair<Image, Image> step1(const Image &noisy, float sigma, int dct_sz,
                         bool adaptive_aggregation) {
  Image result(noisy.rows(), noisy.columns(), noisy.channels());
  Image weights(noisy.rows(), noisy.columns());

  DCTPatch patch(dct_sz, dct_sz, noisy.channels());
  for (int pr = 0; pr <= noisy.rows() - dct_sz; ++pr) {
    for (int pc = 0; pc <= noisy.columns() - dct_sz; ++pc) {
      ExtractPatch(noisy, pr, pc, &patch);
      patch.ToFreq();
      float wP = 0; // adaptive aggregation weight
      for (int chan = 0; chan < noisy.channels(); ++chan) {
        for (int row = 0; row < dct_sz; ++row) {
          for (int col = 0; col < dct_sz; ++col) {
            if (row || col) {
              // Hard thresholding
              if (abs(patch.freq(col, row, chan)) < HARD_THRESHOLD * sigma) {
                patch.freq(col, row, chan) = 0.f;
              }
            }
            // count ALL nonzero frequencies including DC
            wP += abs(patch.freq(col, row, chan))>0.f ? 1.f : 0.f; 
          }
        }
      }
      patch.ToSpace();
      wP = wP>=1 ? 1.f/wP : 1.f;
      if (!adaptive_aggregation)
         wP = 1.f;

      // Aggregation
      for (int ch = 0; ch < noisy.channels(); ++ch) {
        for (int row = 0; row < dct_sz; ++row) {
          for (int col = 0; col < dct_sz; ++col) {
            result.val(col + pc, row + pr, ch) += patch.space(col, row, ch)*wP;
          }
        }
      }
      for (int row = 0; row < dct_sz; ++row) {
        for (int col = 0; col < dct_sz; ++col) {
          weights.val(col + pc, row + pr) += 1.f*wP;
        }
      }
    }
  }
  return {move(result), move(weights)};
}

pair<Image, Image> step2(const Image &noisy, const Image &guide,
                         const float sigma, const int dct_sz,
                         bool adaptive_aggregation) {
  Image result(noisy.rows(), noisy.columns(), noisy.channels());
  Image weights(noisy.rows(), noisy.columns());

  DCTPatch patch(dct_sz, dct_sz, noisy.channels());
  DCTPatch gpatch(dct_sz, dct_sz, noisy.channels());
  for (int pr = 0; pr <= noisy.rows() - dct_sz; ++pr) {
    for (int pc = 0; pc <= noisy.columns() - dct_sz; ++pc) {
      ExtractPatch(noisy, pr, pc, &patch);
      ExtractPatch(guide, pr, pc, &gpatch);
      patch.ToFreq();
      gpatch.ToFreq();
      float wP = 0; // adaptive aggregation weight
      for (int chan = 0; chan < noisy.channels(); ++chan) {
        for (int row = 0; row < dct_sz; ++row) {
          for (int col = 0; col < dct_sz; ++col) {
            if (row || col) {
              // Wiener
              float G = gpatch.freq(col, row, chan);
              float w = (G * G) / (G * G + sigma * sigma);
              patch.freq(col, row, chan) *= w;
              wP += abs(patch.freq(col, row, chan)); // add to weights excluding DC
            }
          }
        }
      }
      patch.ToSpace();

      wP = wP>=1e-6 ? 1.f/wP : 1.f;
      if (!adaptive_aggregation)
         wP = 1.f;

      // Aggregation
      for (int ch = 0; ch < noisy.channels(); ++ch) {
        for (int row = 0; row < dct_sz; ++row) {
          for (int col = 0; col < dct_sz; ++col) {
            result.val(col + pc, row + pr, ch) += patch.space(col, row, ch)*wP;
          }
        }
      }
      for (int row = 0; row < dct_sz; ++row) {
        for (int col = 0; col < dct_sz; ++col) {
          weights.val(col + pc, row + pr) += 1.f*wP;
        }
      }
    }
  }
  return {move(result), move(weights)};
}

// Denoise an image with sliding DCT thresholding.
Image DCTdenoising(const Image &noisy, float sigma, int dct_sz, 
                   bool adaptive_aggregation, int nthreads) {
#ifdef _OPENMP
  if (!nthreads) nthreads = omp_get_max_threads();  // number of threads
#else
  nthreads = 1;
#endif  // _OPENMP

  int r = dct_sz / 2; // half patch size for the padding 
  pair<int, int> tiling = ComputeTiling(noisy.rows(), noisy.columns(),
                                        nthreads);
  vector<Image> noisy_tiles = SplitTiles(ColorTransform(noisy.copy()), r,
                                         dct_sz - r, tiling);
  vector<pair<Image, Image>> result_tiles(nthreads);

#pragma omp parallel for num_threads(nthreads)
  for (int i = 0; i < nthreads; ++i) {
    result_tiles[i] = step1(noisy_tiles[i], sigma, dct_sz,
                            adaptive_aggregation);
  }

  return ColorTransformInverse(MergeTiles(result_tiles, noisy.shape(), r,
                                          dct_sz - r, tiling));
}

// Denoise an image with sliding DCT thresholding.
Image DCTdenoisingGuided(const Image &noisy, const Image &guide, float sigma,
                   int dct_sz, bool adaptive_aggregation, int nthreads) {
#ifdef _OPENMP
  if (!nthreads) nthreads = omp_get_max_threads();  // number of threads
#else
  nthreads = 1;
#endif  // _OPENMP

  int r = dct_sz / 2; // half patch size for the padding
  pair<int, int> tiling = ComputeTiling(noisy.rows(), noisy.columns(),
                                        nthreads);
  vector<Image> noisy_tiles = SplitTiles(ColorTransform(noisy.copy()), r,
                                         dct_sz - r, tiling);
  vector<Image> guide_tiles = SplitTiles(ColorTransform(guide.copy()), r,
                                         dct_sz - r, tiling);
  vector<pair<Image, Image>> result_tiles(nthreads);

#pragma omp parallel for num_threads(nthreads)
  for (int i = 0; i < nthreads; ++i) {
    result_tiles[i] = step2(noisy_tiles[i], guide_tiles[i], sigma, dct_sz,
                            adaptive_aggregation);
  }

  return ColorTransformInverse(MergeTiles(result_tiles, noisy.shape(), r,
                                          dct_sz - r, tiling));
}
