/* Copyright 2023 CMU, Facebook, LANL, MIT, NVIDIA, and Stanford (alphabetical)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flexflow/ops/tree_inc_multihead_self_attention.h"
#include "flexflow/ffconst_utils.h"
#include "flexflow/ops/kernels/inc_multihead_self_attention_kernels.h"
#include "flexflow/ops/kernels/inc_multihead_self_attention_utils.cuh"
#include "flexflow/ops/tree_inc_multihead_self_attention.h"
#include "flexflow/utils/hip_helper.h"
#include <hip/hip_complex.h>
#include <hip/hip_runtime.h>

namespace FlexFlow {

// declare Legion names
using Legion::coord_t;
using Legion::Memory;

#define WARP_SIZE 32

using namespace Kernels::IncMultiHeadAttention;

namespace Kernels {
namespace TreeIncMultiHeadAttention {

template <typename T>
__device__ __forceinline__ T
    WARP_SHFL(unsigned mask, T var, int srcLane, int width = warpSize) {
#ifndef __HIP_PLATFORM_HCC__
  return __shfl_sync(mask, var, srcLane, width);
#else
  return __shfl(var, srcLane, width);
#endif
}

template <typename T>
__device__ __forceinline__ T
    WARP_SHFL_XOR(unsigned mask, T var, int laneMask, int width = warpSize) {
#ifndef __HIP_PLATFORM_HCC__
  return __shfl_xor_sync(mask, var, laneMask, width);
#else
  return __shfl_xor(var, laneMask, width);
#endif
}

template <typename DT,
          int THREADS_PER_BLOCK,
          int Dh,
          int Dh_MAX,
          int THREADS_PER_KEY,
          int THREADS_PER_VALUE>
__global__ void compute_attention_kernel_fused_kernel(
    DT const *query,
    DT const *key_cache,
    DT const *value_cache,
    DT *output_ptr,
    float const scale,
    int const max_seq_length,
    int const max_token_per_batch,
    int per_head_size,
    int hidden_size,
    BatchConfig::PerRequestInfo *request_infos,
    int num_heads,
    int num_requests,
    BatchConfig::BitMask *causalMask,
    bool *request_completed,
    int qk_smem_sz) {

  // q, k
  using Q_vec = typename VEC_K<DT, THREADS_PER_KEY>::Type;
  using K_vec = typename VEC_K<DT, THREADS_PER_KEY>::Type;
  using V_vec = typename VEC_V<DT>::Type;
  using Out_sum = typename Vec_fp32_<V_vec>::Type;

  constexpr int WARPS_PER_BLOCK = THREADS_PER_BLOCK / WARP_SIZE;

  constexpr int K_VEC_SIZE = sizeof(K_vec) / sizeof(DT);
  constexpr int K_ELTS_PER_THREAD = Dh / THREADS_PER_KEY;
  constexpr int K_VECS_PER_THREAD = K_ELTS_PER_THREAD / K_VEC_SIZE;
  // constexpr int QK_ELTS_IN_16B = 16 / sizeof(DT);

  // thread id
  int const tidx = threadIdx.x;
  // head id
  int const head_idx = blockIdx.x;
  // request idx
  int const request_idx = blockIdx.y;

  int const batch_config_request_id =
      request_infos[request_idx].batch_config_request_id;

  int const first_step = 0;

  int const tlength =
      request_infos[batch_config_request_id].first_token_depth_in_request +
      request_infos[batch_config_request_id].num_tokens_in_batch;
  int const qlength =
      request_infos[batch_config_request_id].num_tokens_in_batch;

  BatchConfig::BitMask bitmask = causalMask[batch_config_request_id];

  int first_token_idx = 0;
  for (int r = 0; r < batch_config_request_id; r++) {
    first_token_idx +=
        request_completed[r] ? 0 : request_infos[r].num_tokens_in_batch;
  }

  bool prompt_phase = request_infos[batch_config_request_id].prompt_phase;
  int q_start =
      request_infos[batch_config_request_id].first_token_depth_in_request;

  // shared memory objects
  extern __shared__ char smem_[];

  float *qk_smem = reinterpret_cast<float *>(smem_);
  float *out_smem = reinterpret_cast<float *>(smem_ + qk_smem_sz);

  float qk_max = -FLT_MAX;

  // first WARPS_PER_BLOCK for store qk_max, second WARPS_PER_BLOCK for sum
  __shared__ float red_smem[WARPS_PER_BLOCK * 2];

  const DT *q_ptr = query + first_token_idx * hidden_size * QKV_WEIGHT_NUM +
                    head_idx * per_head_size;
  __shared__ Q_vec q_vecs[THREADS_PER_KEY][K_VECS_PER_THREAD];

  // the start offset of the element eg. (0, 1, 2, 3) * K_VEC_SIZE
  int ki = tidx % THREADS_PER_KEY * K_VEC_SIZE;
  int ki_o = tidx % THREADS_PER_KEY;
  // the first key's offset for this thread
  // ko = 0, 0, 0, 0, 1, 1, 1, 1, ....
  int ko = tidx / THREADS_PER_KEY;
  // load q tensor
  Q_vec q_vec[K_VECS_PER_THREAD];

  constexpr int K_PER_ITER = THREADS_PER_BLOCK / THREADS_PER_KEY;
  // The number of keys per warp.
  constexpr int K_PER_WARP = WARP_SIZE / THREADS_PER_KEY;

  DT const *k_cache_batch =
      key_cache + batch_config_request_id * max_seq_length * hidden_size + ki;

  int ti_end =
      div_up(tlength - first_step, K_PER_WARP) * K_PER_WARP + first_step;

  for (int qi = 0; qi < qlength; qi += 1) {
#pragma unroll
    for (int ii = 0; ii < K_VECS_PER_THREAD; ++ii) {
      q_vecs[ki_o][ii] = *reinterpret_cast<Q_vec const *>(
          q_ptr + (hidden_size * QKV_WEIGHT_NUM * qi) + ki +
          ii * THREADS_PER_KEY * K_VEC_SIZE);

      // if (head_idx == 0 && request_idx == 1 && tidx == 0) {
      //     printf("laod q %d,  %d %.10f\n",
      //     request_idx,
      //            qi,q_vecs[ki_o][ii].x);
      //   }
    }

    __syncthreads();
    for (int ti = ko; ti < ti_end; ti += K_PER_ITER) {
      K_vec k[K_VECS_PER_THREAD];
      int const ti_circ = ti % max_seq_length;

      for (int ii = 0; ii < K_VECS_PER_THREAD; ++ii) {
        int jj = ii * THREADS_PER_KEY * K_VEC_SIZE;
        if (ti < tlength) {
          k[ii] = *reinterpret_cast<K_vec const *>(
              k_cache_batch + ti_circ * hidden_size + head_idx * per_head_size +
              jj);
        }
      }
      float qk = scale * Qk_dot<DT, THREADS_PER_KEY>::dot(q_vecs[ki_o], k);

      if (ti < tlength && tidx % THREADS_PER_KEY == 0) {
        bool const mask =
            prompt_phase ? (qi + q_start < ti)
                         : (ti >= bitmask.non_tree_cache_size &&
                            (!(bitmask.mask[ti - bitmask.non_tree_cache_size] &
                               (1 << qi))));

        qk_max = mask ? qk_max : fmaxf(qk_max, qk);

        // if (head_idx == 0 && !mask) {
        //   printf("tree attn qkqkqkqk request id %d qi%d, ti %d, %.10f, %.10f,
        //   %.10f, %d\n",
        //          request_idx,
        //          qi,
        //          ti,
        //          qk,
        //          q_vecs[ki_o][0].x,
        //          k[0].x,
        //          bitmask.non_tree_cache_size);
        // }
        qk_smem[ti - first_step] = mask ? 0.0f : qk;
      }
    }

    __syncthreads();

#pragma unroll
    for (int mask = WARP_SIZE / 2; mask >= THREADS_PER_KEY; mask /= 2) {
      qk_max = fmaxf(qk_max, WARP_SHFL_XOR(uint32_t(-1), qk_max, mask));
    }

    // Decompose the thread index into warp and lane.
    int const warp = tidx / WARP_SIZE;
    int const lane = tidx % WARP_SIZE;

    // The warp leader writes the max to shared memory.
    if (lane == 0) {
      red_smem[warp] = qk_max;
    }

    // Make sure the products are in shared memory.
    __syncthreads();

    // The warps finalize the reduction.
    qk_max = lane < WARPS_PER_BLOCK ? red_smem[lane] : -FLT_MAX;
#pragma unroll
    for (int mask = WARPS_PER_BLOCK / 2; mask >= 1; mask /= 2) {
      qk_max = fmaxf(qk_max, WARP_SHFL_XOR(uint32_t(-1), qk_max, mask));
    }

    // Broadcast to all the threads in the warp.
    qk_max = WARP_SHFL(uint32_t(-1), qk_max, 0);

    // if (head_idx == 0 && qi == 9 && tidx == 0) {
    //   printf("tree attn first token qk_max %f\n", qk_max);
    // }

    float exp_sum = 0.f;
    for (int ti = first_step + tidx; ti < tlength; ti += THREADS_PER_BLOCK) {
      bool const mask =
          prompt_phase ? (q_start + qi < ti)
                       : (ti >= bitmask.non_tree_cache_size &&
                          (!(bitmask.mask[ti - bitmask.non_tree_cache_size] &
                             (1 << qi))));
      float logit = mask ? 0.0f : __expf(qk_smem[ti - first_step] - qk_max);
      exp_sum += logit;
      qk_smem[ti - first_step] = mask ? 0.0f : logit;
    }

    // Compute the sum.
    exp_sum = block_sum<WARPS_PER_BLOCK>(&red_smem[WARPS_PER_BLOCK], exp_sum);

    // softmax
    float inv_sum = __fdividef(1.f, exp_sum + 1.e-6);
    for (int ti = first_step + tidx; ti < tlength; ti += THREADS_PER_BLOCK) {
      qk_smem[ti - first_step] *= inv_sum;
    }

    __syncthreads();

    // value projection
    constexpr int V_VEC_SIZE = 16 / sizeof(DT);
    // A vector of V elements for the current timestep.
    // using V_vec_k = typename V_vec_k_<DT, V_VEC_SIZE>::Type;
    // using V_vec_acum = typename V_vec_acum_fp32_<V_vec_k>::Type;

    // The value computed by this thread.
    int vo = tidx / THREADS_PER_VALUE;
    // The hidden dimensions computed by this particular thread.
    int vi = tidx % THREADS_PER_VALUE * V_VEC_SIZE;
    constexpr int V_PER_ITER = THREADS_PER_BLOCK / THREADS_PER_VALUE;

    Out_sum out;
    zero(out);

    // The base pointer for the value in the cache buffer.
    DT const *v_cache_batch =
        value_cache + batch_config_request_id * max_seq_length * hidden_size +
        vi;

    if (Dh == Dh_MAX || vi < Dh) {
      for (int ti = first_step + vo; ti < tlength; ti += V_PER_ITER) {
        // Load the values from the cache.
        int const ti_circ = ti % max_seq_length;
        // int const real_cache_idx = topology.real_token_pos[sub_req_idx][ti];
        V_vec v = *reinterpret_cast<V_vec const *>(
            v_cache_batch + ti_circ * hidden_size + head_idx * per_head_size);

        if (ti < tlength) {
          bool const mask =
              prompt_phase
                  ? (q_start + qi < ti)
                  : (ti >= bitmask.non_tree_cache_size &&
                     (!(bitmask.mask[ti - bitmask.non_tree_cache_size] &
                        (1 << qi))));
          float logit = mask ? 0.0f : qk_smem[ti - first_step];
          out = FlexFlow::fma(logit, cast_to_float(v), out);
        }
      }
    }

    //   // Make sure we can start writing to shared memory.
    __syncthreads();

    // Run the final reduction amongst the different groups computing different
    // partial outputs.
    if (Dh == Dh_MAX || vi < Dh) {
#pragma unroll
      for (int active_groups = V_PER_ITER; active_groups >= 2;
           active_groups /= 2) {

        // The midpoint in the number of active groups.
        int midpoint = active_groups / 2;

        // The upper part of active threads store to shared memory.
        if (vo >= midpoint && vo < active_groups && (Dh == Dh_MAX || vi < Dh)) {
          *reinterpret_cast<Out_sum *>(out_smem + (vo - midpoint) * Dh + vi) =
              out;
        }
        __syncthreads();

        // The bottom warps update their values.
        if (vo < midpoint && (Dh == Dh_MAX || vi < Dh)) {
          out = add(*reinterpret_cast<Out_sum const *>(out_smem + vo * Dh + vi),
                    out);
        }
        __syncthreads();
      }
    }

    // Output the final values.
    if (vo == 0 && (Dh == Dh_MAX || vi < Dh)) {
      convert_from_float(*reinterpret_cast<V_vec *>(
                             output_ptr + (first_token_idx + qi) * hidden_size +
                             head_idx * per_head_size + vi),
                         out);
      // if (blockIdx.y == 0 && blockIdx.x == 0 && tidx == 0 && qi == 1) {
      //   printf("tree attn final value, %.9f, %.9f, %.9f, %.9f, %d, %d\n",
      //          out.x,
      //          out.y,
      //          out.z,
      //          out.w,
      //          vi,
      //          (first_token_idx + qi) * hidden_size + head_idx *
      //          per_head_size +
      //              vi);
      // }
    }
  }
}

template <typename DT>
__global__ void commit_tokens_kernel(
    DT const *devQKVProjArray,
    DT *kCache_ptr,
    DT *vCache_ptr,
    TreeVerifyBatchConfig::CommittedTokensInfo const *committedTokenInfos,
    int qProjSize,
    int kProjSize,
    int vProjSize,
    int num_tokens_to_commit,
    int num_active_tokens_in_last_batch,
    int max_seq_len,
    int hidden_size) {

  CUDA_KERNEL_LOOP(i, num_tokens_to_commit * hidden_size) {

    int token_pos = i / (hidden_size);
    int token_idx_in_last_batch = committedTokenInfos[token_pos].token_index;
    int offset = i % hidden_size;
    assert(token_idx_in_last_batch < num_active_tokens_in_last_batch);

    size_t val_idx = token_idx_in_last_batch * QKV_WEIGHT_NUM * hidden_size +
                     hidden_size + offset;

    DT kVal = devQKVProjArray[val_idx];
    DT vVal = devQKVProjArray[val_idx + hidden_size];

    int const req_id = committedTokenInfos[token_pos].request_index;
    int const tok_id = committedTokenInfos[token_pos].token_depth;

    kCache_ptr[req_id * (hidden_size * max_seq_len) + tok_id * hidden_size +
               offset] = kVal;
    vCache_ptr[req_id * (hidden_size * max_seq_len) + tok_id * hidden_size +
               offset] = vVal;
  }
}

template <typename DT>
void commit_tokens(TreeIncMultiHeadSelfAttentionMeta const *m,
                   TreeVerifyBatchConfig const *bc,
                   hipStream_t stream) {
  int num_tokens_to_commit = bc->num_tokens_to_commit;
  if (num_tokens_to_commit > 0) {
    int parallelism = m->hidden_size * KV_WEIGHT_NUM * num_tokens_to_commit;
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(commit_tokens_kernel<DT>),
        GET_BLOCKS(parallelism),
        min(CUDA_NUM_THREADS, parallelism),
        0,
        stream,
        static_cast<DT *>(m->devQKVProjArray),
        static_cast<DT *>(m->keyCache),
        static_cast<DT *>(m->valueCache),
        m->committed_token_infos,
        m->qProjSize,
        m->kProjSize,
        m->vProjSize,
        num_tokens_to_commit,
        m->num_active_infr_tokens, // number of active tokens in previous batch
        BatchConfig::max_sequence_length() +
            BatchConfig::max_spec_tree_token_num(),
        m->hidden_size);
  }
}

template <typename DT>
__global__ void update_tree_branch_kv_cache(
    DT const *devQKVProjArray,
    DT *kCache_ptr,
    DT *vCache_ptr,
    TreeVerifyBatchConfig::PerTokenInfo const *tokenInfos,
    int qProjSize,
    int kProjSize,
    int vProjSize,
    int num_tokens_in_branch,
    int processed_tokens_in_batch,
    int total_tokens_in_batch,
    int max_seq_len,
    int hidden_size) {
  CUDA_KERNEL_LOOP(i, num_tokens_in_branch * hidden_size) {

    int token_idx = i / (hidden_size);
    int offset = i % hidden_size;

    token_idx += processed_tokens_in_batch; // get index in the whole batch
    size_t val_idx =
        token_idx * QKV_WEIGHT_NUM * hidden_size + hidden_size + offset;

    DT kVal = devQKVProjArray[val_idx];
    DT vVal = devQKVProjArray[val_idx + hidden_size];

    int const req_id = tokenInfos[token_idx].request_index;
    int const tok_id = tokenInfos[token_idx].abs_depth_in_request;
    kCache_ptr[req_id * (hidden_size * max_seq_len) + tok_id * hidden_size +
               offset] = kVal;
    vCache_ptr[req_id * (hidden_size * max_seq_len) + tok_id * hidden_size +
               offset] = vVal;
  }
}

template <typename DT>
__global__ void update_tree_branch_kv_cache_fused(
    DT const *devQKVProjArray,
    DT *kCache_ptr,
    DT *vCache_ptr,
    TreeVerifyBatchConfig::PerTokenInfo const *tokenInfos,
    BatchConfig::PerRequestInfo *request_infos,
    int qProjSize,
    int kProjSize,
    int vProjSize,
    int num_new_tokens,
    int max_seq_len,
    int hidden_size) {
  CUDA_KERNEL_LOOP(i, num_new_tokens * hidden_size) {

    int token_idx = i / hidden_size;
    int offset = i % hidden_size;
    size_t val_idx =
        token_idx * QKV_WEIGHT_NUM * hidden_size + hidden_size + offset;

    DT kVal = devQKVProjArray[val_idx];
    DT vVal = devQKVProjArray[val_idx + hidden_size];

    int const req_id = tokenInfos[token_idx].request_index;
    // int const tok_id = tokenInfos[token_idx].abs_depth_in_request;

    int const request_token_offset =
        request_infos[req_id].first_token_offset_in_batch;
    int const first_token_depth =
        request_infos[req_id].first_token_depth_in_request;

    // if(i % hidden_size == 0){
    //   printf("update token request id: %d, %d, %d  real id %d, value%.10f\n",
    //   req_id, token_idx, request_token_offset,(token_idx + first_token_depth
    //   - request_token_offset), kVal);
    // }
    kCache_ptr[req_id * (hidden_size * max_seq_len) +
               (token_idx + first_token_depth - request_token_offset) *
                   hidden_size +
               offset] = kVal;
    vCache_ptr[req_id * (hidden_size * max_seq_len) +
               (token_idx + first_token_depth - request_token_offset) *
                   hidden_size +
               offset] = vVal;
  }
}

template <typename DT>
__global__ void tree_fill_entries_above_diagonal(DT *matrix,
                                                 size_t new_tokens,
                                                 size_t total_tokens_in_request,
                                                 size_t num_q_heads,
                                                 DT value) {
  CUDA_KERNEL_LOOP(i, new_tokens * total_tokens_in_request * num_q_heads) {
    // size_t head_idx = i / (new_tokens * total_tokens_in_request);
    size_t src_idx = (i / new_tokens) % total_tokens_in_request;
    size_t dst_idx = i % new_tokens + total_tokens_in_request - new_tokens;
    // Casual Mask
    if (src_idx > dst_idx) {
      matrix[i] = value;
    }
  }
}

#define LAUNCH_TREE_VERIFY_ATTENTION_SCORE_KERNEL(                             \
    DT, Dh, Dh_MAX, THDS_PER_KEY, THDS_PER_VALUE, THDS_PER_BLOCK, stream)      \
  smem_size_in_bytes_tree<DT>(m->qProjSize,                                    \
                              BatchConfig::max_sequence_length() +             \
                                  BatchConfig::max_spec_tree_token_num(),      \
                              THDS_PER_VALUE,                                  \
                              THDS_PER_BLOCK,                                  \
                              bc,                                              \
                              smem_sz);                                        \
  compute_attention_kernel_fused_kernel<DT,                                    \
                                        THDS_PER_BLOCK,                        \
                                        Dh,                                    \
                                        Dh_MAX,                                \
                                        THDS_PER_KEY,                          \
                                        THDS_PER_VALUE>                        \
      <<<grid, THDS_PER_BLOCK, smem_sz[1], stream>>>(                          \
          static_cast<DT *>(m->devQKVProjArray),                               \
          static_cast<DT *>(m->keyCache),                                      \
          static_cast<DT *>(m->valueCache),                                    \
          output_ptr,                                                          \
          scale,                                                               \
          BatchConfig::max_sequence_length() +                                 \
              BatchConfig::BatchConfig::max_spec_tree_token_num(),             \
          BatchConfig::max_tokens_per_batch(),                                 \
          m->qProjSize,                                                        \
          m->hidden_size,                                                      \
          m->request_infos,                                                    \
          m->num_q_heads,                                                      \
          bc->num_active_requests(),                                           \
          m->causalMask,                                                       \
          m->request_completed,                                                \
          smem_sz[0])

template <typename DT>
void compute_attention_kernel_fused(TreeIncMultiHeadSelfAttentionMeta const *m,
                                    TreeVerifyBatchConfig const *bc,
                                    DT *output_ptr,
                                    hipStream_t stream) {

  // update the kv cache
  //  update K-V cache
  int num_new_tokens = bc->num_active_tokens();
  int parallelism = m->hidden_size * num_new_tokens;
  update_tree_branch_kv_cache_fused<<<GET_BLOCKS(parallelism),
                                      min(CUDA_NUM_THREADS, parallelism),
                                      0,
                                      stream>>>(
      static_cast<DT *>(m->devQKVProjArray),
      static_cast<DT *>(m->keyCache),
      static_cast<DT *>(m->valueCache),
      m->token_infos,
      m->request_infos,
      m->qProjSize,
      m->kProjSize,
      m->vProjSize,
      num_new_tokens,
      BatchConfig::max_sequence_length() +
          BatchConfig::max_spec_tree_token_num(),
      m->hidden_size);

  dim3 grid(m->num_q_heads, bc->num_active_requests());
  int const per_head_size = m->qProjSize;
  float scale = (*m->qk_prod_scaling) ? 1.0f / sqrt(m->kProjSize) : 1.0f;
  // 0->qk production size, 1->total shared size
  int smem_sz[2];
  if (per_head_size == 64) {
    constexpr int THREADS_PER_VALUE_64 = threads_per_value_t<DT, 64>::value;
    LAUNCH_TREE_VERIFY_ATTENTION_SCORE_KERNEL(
        DT, 64, 64, 4, THREADS_PER_VALUE_64, 128, stream);
  } else if (per_head_size == 128) {
    constexpr int THREADS_PER_VALUE_128 = threads_per_value_t<DT, 128>::value;
    LAUNCH_TREE_VERIFY_ATTENTION_SCORE_KERNEL(
        DT, 128, 128, 4, THREADS_PER_VALUE_128, 128, stream);
  } else {
    assert(false && "a unsupported head size");
  }
}

template <typename DT>
void inference_kernel(TreeIncMultiHeadSelfAttentionMeta *m,
                      TreeVerifyBatchConfig const *bc,
                      int shard_id,
                      DT const *qkv_ptr,
                      DT *output_ptr,
                      hipStream_t stream) {

  // copy committed tokens info to GPU for the commit_tokens kernel
  // Note that m->num_active_infr_tokens stores the number of active
  // tokens in the previous batch, which is needed for committing
  // keys/values to the key-value cache
  // std::cout << "tokens to be committed: " << bc->num_tokens_to_commit <<
  // "\n";

  commit_tokens<DT>(m, bc, stream);

  // After commit we update m->num_active_infr_tokens to be the number of active
  // tokens for the current batch
  m->num_active_infr_tokens = bc->num_active_infr_tokens();

  // phase 0: copy calculated qkv into devQKVProjArray
  // [qProjSize, num_heads, 3, num_new_tokens]
  size_t qkv_proj_size =
      m->qProjSize * m->num_q_heads * QKV_WEIGHT_NUM * bc->num_active_tokens();

  hipMemcpyAsync(m->devQKVProjArray,
                 qkv_ptr,
                 qkv_proj_size *
                     sizeof(DT), // is this right, do we need layers etc here
                 hipMemcpyDeviceToDevice,
                 stream);

  // phase 1: Implement kernel to compute KQV for input tokens
  // TODO WARNING: this is commented out only because we are fixing the inc_attn
  // first
  compute_qkv_kernel(
      m, bc, shard_id, static_cast<DT *>(m->devQKVProjArray), stream);

  // phase 2: No need to update key/val cache
  compute_attention_kernel_fused<DT>(
      m, bc, static_cast<DT *>(m->attn_heads), stream);

  int processed_tokens_in_batch = bc->num_active_tokens();

  int num_tokens = bc->num_active_tokens();
  hipMemcpyAsync(output_ptr,
                 m->attn_heads,
                 m->oProjSize * num_tokens * sizeof(DT),
                 hipMemcpyDeviceToDevice,
                 stream);
}

} // namespace TreeIncMultiHeadAttention
} // namespace Kernels

/*static*/
void TreeIncMultiHeadSelfAttention::inference_kernel_wrapper(
    TreeIncMultiHeadSelfAttentionMeta *m,
    TreeVerifyBatchConfig const *bc,
    int shard_id,
    GenericTensorAccessorR const &input,
    GenericTensorAccessorW const &output) {
  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));

  hipEvent_t t_start, t_end;
  if (m->profiling) {
    checkCUDA(hipEventCreate(&t_start));
    checkCUDA(hipEventCreate(&t_end));
    checkCUDA(hipEventRecord(t_start, stream));
  }

  assert(input.data_type == output.data_type);

  if (input.data_type == DT_HALF) {
    Kernels::TreeIncMultiHeadAttention::inference_kernel(
        m, bc, shard_id, input.get_half_ptr(), output.get_half_ptr(), stream);
  } else if (input.data_type == DT_FLOAT) {
    Kernels::TreeIncMultiHeadAttention::inference_kernel(
        m, bc, shard_id, input.get_float_ptr(), output.get_float_ptr(), stream);
  } else {
    assert(false && "Unspported data type");
  }

  if (m->profiling) {
    checkCUDA(hipEventRecord(t_end, stream));
    checkCUDA(hipEventSynchronize(t_end));
    float elapsed = 0;
    checkCUDA(hipEventElapsedTime(&elapsed, t_start, t_end));
    checkCUDA(hipEventDestroy(t_start));
    checkCUDA(hipEventDestroy(t_end));
    printf("TreeIncMultiHeadSelfAttention forward time = %.2fms\n", elapsed);
  }
}

TreeIncMultiHeadSelfAttentionMeta::TreeIncMultiHeadSelfAttentionMeta(
    FFHandler handler,
    TreeIncMultiHeadSelfAttention const *attn,
    MemoryAllocator &gpu_mem_allocator,
    int num_samples,
    int _num_q_heads,
    int _num_kv_heads)
    : IncMultiHeadSelfAttentionMeta(handler,
                                    TREE_VERIFY_MODE,
                                    attn,
                                    attn->qSize,
                                    attn->kSize,
                                    attn->vSize,
                                    attn->qProjSize,
                                    attn->kProjSize,
                                    attn->vProjSize,
                                    attn->oProjSize,
                                    attn->rotary_embedding_meta,
                                    attn->scaling_query,
                                    attn->qk_prod_scaling,
                                    attn->position_bias,
                                    attn->scaling_factor,
                                    gpu_mem_allocator,
                                    num_samples,
                                    attn->num_q_heads,
                                    attn->num_kv_heads,
                                    _num_q_heads,
                                    _num_kv_heads,
                                    attn->quantization_type,
                                    attn->offload),
      num_active_infr_tokens(0) {
  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));
  checkCUDNN(miopenSetStream(handler.dnn, stream));

  // allocate memory for the seqArray and reserve space
  {

    causalMask = static_cast<BatchConfig::BitMask *>(
        handler.batch_config_metadata->causalMask);
    committed_token_infos =
        static_cast<TreeVerifyBatchConfig::CommittedTokensInfo *>(
            handler.batch_config_metadata->committed_tokens);
    request_completed =
        static_cast<bool *>(handler.batch_config_metadata->request_completed);
  }

  checkCUDA(hipStreamSynchronize(stream));
}

TreeIncMultiHeadSelfAttentionMeta::~TreeIncMultiHeadSelfAttentionMeta(void) {
  if (committed_token_reserve_inst != Realm::RegionInstance::NO_INST) {
    committed_token_reserve_inst.destroy();
  }
}

}; // namespace FlexFlow
