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

#include "flexflow/utils/file_loader.h"
#include "flexflow/ffconst_utils.h"
#include "flexflow/inference.h"
#include "flexflow/model.h"

#include <vector>
using namespace std;

using namespace Legion;

FileDataLoader::FileDataLoader(std::string _prompts_filepath,
                               std::string _weights_folder,
                               int _num_heads,
                               int _num_kv_heads,
                               size_t _hidden_dim,
                               size_t _qkv_inner_dim,
                               int _tensor_parallelism_degree,
                               bool _use_full_precision)
    : prompts_filepath(_prompts_filepath), weights_folder(_weights_folder),
      num_heads(_num_heads), num_kv_heads(_num_kv_heads),
      hidden_dim(_hidden_dim), qkv_inner_dim(_qkv_inner_dim),
      tensor_parallelism_degree(_tensor_parallelism_degree),
      use_full_precision(_use_full_precision){};

BatchConfig::TokenId *FileDataLoader::generate_requests(int num, int length) {

  BatchConfig::TokenId *prompts =
      (BatchConfig::TokenId *)malloc(sizeof(BatchConfig::TokenId) * 40);
  std::ifstream in(prompts_filepath, std::ios::in | std::ios::binary);
  int size = num * length;
  std::vector<long> host_array(size);
  size_t loaded_data_size = sizeof(long) * size;

  in.seekg(0, in.end);
  in.seekg(0, in.beg);
  in.read((char *)host_array.data(), loaded_data_size);

  size_t in_get_size = in.gcount();
  if (in_get_size != loaded_data_size) {
    std::cout << "load data error" << std::endl;
    return prompts;
  }

  assert(size == host_array.size());
  int index = 0;
  int data_index = 0;

  for (auto v : host_array) {
    prompts[data_index++] = v;
  }
  in.close();
  return prompts;
};

std::string removeGuidOperatorName(std::string const &input) {
  // Find the last underscore in the string
  size_t underscorePos = input.find_last_of('_');

  if (underscorePos != std::string::npos) {
    // Remove the underscore and the characters after it
    return input.substr(0, underscorePos);
  } else {
    // No underscore found, return the original string
    return input;
  }
}

template <typename DT>
void load_attention_o_proj_bias_to_dense_v2(DT *ptr,
                                            int num_heads,
                                            int num_kv_heads,
                                            size_t hidden_dim,
                                            size_t qkv_inner_dim,
                                            std::string layer_name,
                                            std::string weights_folder) {
  std::string filename = layer_name + ".o_proj.bias";

  int file_index = 0;

  // now only opt use this.
  // assert(num_heads == num_kv_heads);
  int idx = 0;

  std::cout << "Loading weight file " << filename << std::endl;
  std::string weight_filepath = join_path({weights_folder, filename});

  int n_heads = num_heads;

  int replicate_num = num_heads / num_kv_heads;

  size_t out_partial_size = hidden_dim;
  size_t partial_size = out_partial_size;
  std::ifstream in(weight_filepath, std::ios::in | std::ios::binary);
  assert(in.good() && "incorrect bias file path");
  std::vector<DT> host_array(partial_size);
  size_t loaded_data_size = sizeof(DT) * partial_size;
  in.seekg(0, in.end);
  in.seekg(0, in.beg);
  in.read((char *)host_array.data(), loaded_data_size);
  size_t in_get_size = in.gcount();

  if (in_get_size != loaded_data_size) {
    printf(
        "load bias data error: in_get_size (%lu) != loaded_data_size (%lu)\n",
        in_get_size,
        loaded_data_size);
    assert(false);
  }
  assert(partial_size == host_array.size());

  size_t data_index = 0;

  for (int i = 0; i < partial_size; i++) {
    ptr[i] = host_array.at(data_index);
    data_index++;
  }

  in.close();
}

template <typename DT>
void load_attention_bias_v2(DT *ptr,
                            int num_heads,
                            int num_kv_heads,
                            size_t hidden_dim,
                            size_t qkv_inner_dim,
                            bool final_bias,
                            std::string layer_name,
                            std::string weights_folder,
                            int tp_degree) {
  std::string q_file = layer_name + ".q_proj.bias";
  std::string k_file = layer_name + ".k_proj.bias";
  std::string v_file = layer_name + ".v_proj.bias";
  std::vector<std::string> bias_files = {q_file, k_file, v_file};

  // linear layer weights: [output_size, input_size]
  // bias layer weights: [output_size]
  // Q,K,V projection weights: [head_dim*num_heads, hidden_size] = [768, 768]
  // QKV bias weights: [head_dim*num_heads] = [768], organized as: [head_dim_0,
  // head_dim_1, ...]

  // need to rearrange: [[q_heads_shard_0], [k_heads_shard_0],
  // [v_heads_shard_0], ..., [q_heads_shard_n], [k_heads_shard_n],
  // [v_heads_shard_n]] where n = tp_degree
  assert(num_heads % tp_degree == 0);
  assert(num_kv_heads % tp_degree == 0);
  assert(hidden_dim % num_heads == 0);
  assert(qkv_inner_dim == hidden_dim / num_heads);
  size_t q_heads_per_shard = num_heads / tp_degree;
  size_t kv_heads_per_shard = num_kv_heads / tp_degree;
  size_t shard_chunk_size =
      (q_heads_per_shard + 2 * kv_heads_per_shard) * qkv_inner_dim;

  int file_index = 0;
  for (auto filename : bias_files) {
    std::cout << "Loading weight file " << filename << std::endl;
    std::string weight_filepath = join_path({weights_folder, filename});

    int n_heads = file_index == 0 ? num_heads : num_kv_heads;
    assert(n_heads % tp_degree == 0);
    int heads_per_shard = n_heads / tp_degree;
    int qkv_prev_heads_cur_shard =
        (file_index == 2) ? num_heads + num_kv_heads : file_index * num_heads;
    assert(qkv_prev_heads_cur_shard % tp_degree == 0);
    qkv_prev_heads_cur_shard /= tp_degree;

    // load into memory first
    size_t bias_size = qkv_inner_dim * n_heads;
    std::ifstream in(weight_filepath, std::ios::in | std::ios::binary);
    assert(in.good() && "incorrect bias file path");
    std::vector<DT> host_array(bias_size);
    size_t loaded_data_size = sizeof(DT) * bias_size;
    in.seekg(0, in.beg);
    in.read((char *)host_array.data(), loaded_data_size);
    size_t in_get_size = in.gcount();
    if (in_get_size != loaded_data_size) {
      printf(
          "load bias data error: in_get_size (%lu) != loaded_data_size (%lu)\n",
          in_get_size,
          loaded_data_size);
      assert(false);
    }
    assert(bias_size == host_array.size());

    // now copy chunks into ptr
    for (int i = 0; i < n_heads; i++) {
      int shard_idx = i / heads_per_shard;
      for (int j = 0; j < qkv_inner_dim; j++) {
        int src_idx = i * qkv_inner_dim + j;
        int dst_idx = shard_idx * shard_chunk_size +
                      qkv_prev_heads_cur_shard * qkv_inner_dim +
                      (i % heads_per_shard) * qkv_inner_dim + j;
        ptr[dst_idx] = host_array.at(src_idx);
      }
    }
    file_index++;
    in.close();
  }
}

template <typename DT>
void load_attention_weights_to_dense_v2(DT *ptr,
                                        int num_heads,
                                        int num_kv_heads,
                                        size_t hidden_dim,
                                        size_t qkv_inner_dim,
                                        std::string layer_name,
                                        std::string weights_folder,
                                        size_t volume,
                                        int tensor_parallelism_degree,
                                        bool load_o_proj) {
  // layers_0_attention_wq_weight
  // layers_0_self_attn_q_proj_weight
  std::string q_file = layer_name + ".q_proj.weight";
  std::string k_file = layer_name + ".k_proj.weight";
  std::string v_file = layer_name + ".v_proj.weight";
  std::string o_file = layer_name + ".o_proj.weight";
  std::vector<std::string> weight_filenames = {q_file, k_file, v_file};
  int file_index = 0;

  int base_index = 0;
  size_t single_proj_size =
      hidden_dim *
      qkv_inner_dim; // size of each of Q,K,V,O weights for a single head
  size_t one_weight_file_size =
      num_heads * single_proj_size; // size of each of Q/K/V/O for all heads

  size_t q_size = one_weight_file_size, o_size = one_weight_file_size;
  size_t k_size = single_proj_size * num_kv_heads,
         v_size = single_proj_size * num_kv_heads;

  size_t k_replicate_size = one_weight_file_size;
  size_t v_replicate_size = one_weight_file_size;

  int replicate_num = num_heads / num_kv_heads;

  // stride for q, k, v, o
  size_t stride_size = (q_size + v_replicate_size + k_replicate_size) /
                       tensor_parallelism_degree;
  if (!load_o_proj) {
    for (auto filename : weight_filenames) {
      std::cout << "Loading weight file " << filename << std::endl;
      std::string weight_filepath = join_path({weights_folder, filename});

      int data_index = 0;
      size_t partial_size = (file_index == 0 || file_index == 3)
                                ? one_weight_file_size
                                : single_proj_size * num_kv_heads;
      size_t one_partition_size =
          one_weight_file_size / tensor_parallelism_degree;

      std::ifstream in(weight_filepath, std::ios::in | std::ios::binary);
      if (!in.good()) {
        std::cout << "Could not open file: " << weight_filepath << std::endl;
      }
      assert(in.good() && "incorrect weight file path");
      std::vector<DT> host_array(partial_size);
      size_t loaded_data_size = sizeof(DT) * partial_size;
      in.seekg(0, in.end);
      in.seekg(0, in.beg);
      in.read((char *)host_array.data(), loaded_data_size);
      size_t in_get_size = in.gcount();

      if (in_get_size != loaded_data_size) {
        std::cout << "load attention data error " << in_get_size << ", "
                  << loaded_data_size << ", " << file_index << ", "
                  << weight_filepath << "\n";
        assert(false && "data size mismatch");
      }
      // wq, wk, wo
      if (file_index == 0) {
        for (int i = 0; i < tensor_parallelism_degree; i++) {
          for (int j = 0; j < one_partition_size; j++) {
            ptr[base_index + i * stride_size + j] = host_array.at(data_index++);
          }
        }
      } else {
        for (int i = 0; i < num_heads; i++) {
          int kv_idx = i / (num_heads / num_kv_heads);
          int head_idx = i % (num_heads / tensor_parallelism_degree);
          int tp_idx = (i / (num_heads / tensor_parallelism_degree));
          for (int j = 0; j < single_proj_size; j++) {
            ptr[base_index + tp_idx * stride_size +
                single_proj_size * head_idx + j] =
                host_array.at(kv_idx * single_proj_size + j);
          }
        }
      }
      // std::cout << "host array going out of scope, releasing" << endl;
      base_index += one_partition_size;
      file_index++;
    }
    assert(base_index == (q_size + k_replicate_size + v_replicate_size) /
                             tensor_parallelism_degree);
  } else {
    std::cout << "Loading weight file " << o_file << std::endl;
    std::string weight_filepath = join_path({weights_folder, o_file});

    std::ifstream in(weight_filepath, std::ios::in | std::ios::binary);
    if (!in.good()) {
      std::cout << "Could not open file: " << weight_filepath << std::endl;
    }
    assert(in.good() && "incorrect weight file path");
    std::vector<DT> host_array(one_weight_file_size);
    size_t loaded_data_size = sizeof(DT) * one_weight_file_size;
    in.seekg(0, in.end);
    in.seekg(0, in.beg);
    in.read((char *)host_array.data(), loaded_data_size);
    size_t in_get_size = in.gcount();

    DT temp;

    for (int i = 0; i < one_weight_file_size; i++) {
      temp = host_array.at(i);
    }

    // std::cout<<"o_proj loaded into host array, total size:
    // "<<one_weight_file_size<<std::endl;

    if (in_get_size != loaded_data_size) {
      std::cout << "load data error" << std::endl;
      assert(false);
    }
    assert(one_weight_file_size == host_array.size());
    int data_index = 0;

    // std::cout<<"read data size checked"<<std::endl;

    for (int i = 0; i < one_weight_file_size; i++) {
      ptr[i] = temp;
    }

    // std::cout<<"ptr allocation good"<<std::endl;

    int one_partition_size =
        qkv_inner_dim * (num_heads / tensor_parallelism_degree);
    for (int i = 0; i < one_weight_file_size; i++) {
      ptr[i] = host_array.at(data_index++);
    }

    in.close();

    assert(data_index == one_weight_file_size);
    // std::cout << "Loaded weight file " << o_file << std::endl;
  }
}

template <typename DT>
void load_from_file(DT *ptr, size_t size, std::string filepath) {
  std::ifstream in(filepath, std::ios::in | std::ios::binary);
  if (!in.good()) {
    std::cout << "Could not open file: " << filepath << std::endl;
  }
  assert(in.good() && "incorrect weight file path");
  std::vector<DT> host_array(size);
  size_t loaded_data_size = sizeof(DT) * size;
  in.seekg(0, in.end);
  in.seekg(0, in.beg);
  in.read((char *)host_array.data(), loaded_data_size);

  size_t in_get_size = in.gcount();
  if (in_get_size != loaded_data_size) {
    std::cout << "load weight data error " << in_get_size << ", "
              << loaded_data_size << ", " << sizeof(DT) << std::endl;
    assert(false);
  }
  assert(size == host_array.size());

  // normal
  long data_index = 0;
  for (auto v : host_array) {
    ptr[data_index++] = v;
  }
  in.close();
}

void FileDataLoader::load_positions(FFModel *ff,
                                    Tensor pt,
                                    ParallelTensor position_pt,
                                    int max_seq_length,
                                    int offset) {
  size_t volume = 1;
  std::vector<int> dims_vec;
  for (int i = 0; i < pt->num_dims; i++) {
    volume *= pt->dims[i];
    dims_vec.push_back(pt->dims[i]);
  }

  // load data;
  int *data = (int *)malloc(sizeof(int) * volume);
  for (int i = 0; i < volume; i++) {
    data[i] = i % max_seq_length + offset;
  }
  // set tensor

  // ParallelTensor position_pt;

  // ff->get_parallel_tensor_from_tensor(pt, position_pt);
  position_pt->set_tensor<int>(ff, dims_vec, data);
}

//--------------------- quantization functions ----------------------
// the data layout is 32 * quantized data + 1 scaling factor + 1 offset factor
// in the decompression mode, the real data = quantized data * scaling factor +
// offset

void load_attention_weights_quantized(char *ptr,
                                      int num_heads,
                                      size_t hidden_dim,
                                      size_t qkv_inner_dim,
                                      std::string layer_name,
                                      std::string weights_folder,
                                      DataType data_type,
                                      bool use_full_precision) {
  std::string q_file = layer_name + ".q_proj.weight";
  std::string k_file = layer_name + ".k_proj.weight";
  std::string v_file = layer_name + ".v_proj.weight";
  std::string o_file = layer_name + ".o_proj.weight";
  std::vector<std::string> weight_filenames = {q_file, k_file, v_file, o_file};

  int file_index = 0;

  size_t single_proj_size =
      hidden_dim *
      qkv_inner_dim; // size of each of Q,K,V,O weights for a single head
  size_t one_weight_file_size =
      num_heads * single_proj_size; // size of each of Q/K/V/O for all heads

  // q, k, v, o -> 0, 1, 2, 3
  for (auto filename : weight_filenames) {
    std::cout << "Loading weight file " << filename << std::endl;
    std::string weight_filepath = join_path({weights_folder, filename});

    size_t partial_size = one_weight_file_size;
    std::ifstream in(weight_filepath, std::ios::in | std::ios::binary);
    if (!in.good()) {
      std::cout << "Could not open file: " << weight_filepath << std::endl;
    }
    assert(in.good() && "incorrect weight file path");
    std::vector<char> host_array(partial_size);
    size_t loaded_data_size = sizeof(char) * partial_size;
    in.seekg(0, in.end);
    in.seekg(0, in.beg);
    in.read((char *)host_array.data(), loaded_data_size);
    size_t in_get_size = in.gcount();

    if (in_get_size != loaded_data_size) {
      std::cout << "load data error";
      return;
    }
    assert(partial_size == host_array.size());

    size_t one_head_size = data_type == DT_INT8
                               ? hidden_dim * (hidden_dim / num_heads)
                               : hidden_dim * (hidden_dim / num_heads) / 2;

    size_t data_index = 0;
    for (int i = 0; i < num_heads; i++) {
      size_t start_index = i * one_head_size * 4 + file_index * one_head_size;
      for (size_t j = start_index; j < start_index + one_head_size; j++) {
        if (data_type == DT_INT4) {
          char v1 = host_array.at(data_index);
          char v2 = host_array.at(data_index + 1);
          ptr[j] = (v2 & 0XF) | (v1 << 4);
          data_index += 2;
        } else {
          ptr[j] = host_array.at(data_index);
          data_index += 1;
        }
      }
    }
    file_index++;
    in.close();
  }

  // load scale and offset to the end of weight tensor
  // the layout is like |values * 32 heads|offset|scale|
  size_t offset = data_type == DT_INT8 ? one_weight_file_size * 4
                                       : (one_weight_file_size * 4) / 2;
  for (auto filename : weight_filenames) {
    std::cout << "Loading weight file " << filename << std::endl;
    std::string weight_filepath = join_path({weights_folder, filename});

    for (int i = 0; i < 2; i++) {
      std::string meta_file =
          i == 0 ? (weight_filepath + "_offset") : (weight_filepath + "_scale");
      size_t partial_size =
          one_weight_file_size / INT4_NUM_OF_ELEMENTS_PER_GROUP;
      std::ifstream in(meta_file, std::ios::in | std::ios::binary);
      if (!in.good()) {
        std::cout << "Could not open file: " << meta_file << std::endl;
      }
      assert(in.good() && "incorrect weight file path");

      if (use_full_precision) {
        // float
        std::vector<float> host_array(partial_size);
        size_t loaded_data_size = sizeof(float) * partial_size;
        in.seekg(0, in.end);
        in.seekg(0, in.beg);
        in.read((char *)host_array.data(), loaded_data_size);
        size_t in_get_size = in.gcount();

        if (in_get_size != loaded_data_size) {
          std::cout << "load data error";
          return;
        }
        assert(partial_size == host_array.size());

        for (auto v : host_array) {
          *(float *)(ptr + offset) = v;
          offset += sizeof(float);
        }
      } else {
        // half
        std::vector<half> host_array(partial_size);
        size_t loaded_data_size = sizeof(half) * partial_size;
        in.seekg(0, in.end);
        in.seekg(0, in.beg);
        in.read((char *)host_array.data(), loaded_data_size);
        size_t in_get_size = in.gcount();

        if (in_get_size != loaded_data_size) {
          std::cout << "load data error";
          return;
        }
        assert(partial_size == host_array.size());
        for (auto v : host_array) {
          *(half *)(ptr + offset) = v;
          offset += sizeof(half);
        }
      }
    }
  }
}

void load_from_quantized_file(char *ptr,
                              size_t size,
                              std::string filename,
                              DataType data_type,
                              bool use_full_precision) {
  assert(data_type == DT_INT4 || data_type == DT_INT8);

  std::string value_file = filename;
  std::string offset_file = filename + "_offset";
  std::string scaling_file = filename + "_scale";
  size_t value_size = 0, offset_size = 0, scaling_size = 0;

  if (data_type == DT_INT4) {
    // float/half + 4bit quantization
    // size1 = volume / 2, size2 = volume / 32 * (sizeof(DT)), size3 = size2
    value_size = 2 * (use_full_precision ? (size * 2 / 3) : (size * 4 / 5));
    offset_size = use_full_precision ? (size / 6) : (size / 10);
    scaling_size = use_full_precision ? (size / 6) : (size / 10);
  } else if (data_type == DT_INT8) {
    // float/half + 8bit quantization
    // size1 = volume * 1, size2 = volume / 32 * (sizeof(DT)), size3 = size2
    value_size = use_full_precision ? (size * 4 / 5) : (size * 8 / 9);
    offset_size = use_full_precision ? (size / 10) : (size / 18);
    scaling_size = use_full_precision ? (size / 10) : (size / 18);
  }

  std::vector<std::string> quantized_files = {
      value_file, offset_file, scaling_file};
  std::vector<size_t> quantized_sizes = {value_size, offset_size, scaling_size};

  int file_idx = 0;
  long data_index = 0;
  for (auto file : quantized_files) {
    std::ifstream in(file, std::ios::in | std::ios::binary);
    if (!in.good()) {
      std::cout << "Could not open file: " << file << std::endl;
    }
    assert(in.good() && "incorrect weight file path");

    // value file, every element is in one byte
    if (file_idx == 0) {
      size = quantized_sizes.at(file_idx);
      std::vector<char> host_array(size);
      size_t loaded_data_size = size;
      in.seekg(0, in.end);
      in.seekg(0, in.beg);
      in.read((char *)host_array.data(), loaded_data_size);

      size_t in_get_size = in.gcount();
      if (in_get_size != loaded_data_size) {
        std::cout << "load weight data error quantized" << in_get_size << ", "
                  << loaded_data_size << ", " << sizeof(char) << std::endl;
        return;
      }
      assert(size == host_array.size());

      // normal
      size_t idx = 0;
      while (idx < host_array.size()) {
        if (data_type == DT_INT4) {
          // pack 2 elements into one byte
          char v1 = host_array.at(idx);
          char v2 = host_array.at(idx + 1);
          // v1 in first 4 bit and v2 in last 4 bit;
          ptr[data_index++] = (v2 & 0XF) | (v1 << 4);
          idx += 2;
        } else {
          ptr[data_index++] = host_array.at(idx++);
        }
      }
    } else if (use_full_precision) {
      // load offset/scale in float type;
      size = quantized_sizes.at(file_idx);
      std::vector<float> host_array(size / sizeof(float));
      size_t loaded_data_size = size;
      in.seekg(0, in.end);
      in.seekg(0, in.beg);
      in.read((char *)host_array.data(), loaded_data_size);

      size_t in_get_size = in.gcount();
      if (in_get_size != loaded_data_size) {
        std::cout << "load weight data error scale/offset" << in_get_size
                  << ", " << loaded_data_size << ", " << sizeof(float) << ", "
                  << file << ", " << size << std::endl;
        return;
      }
      assert(size / sizeof(float) == host_array.size());
      for (auto v : host_array) {
        *(float *)(ptr + data_index) = v;
        data_index += sizeof(float);
      }

    } else {
      // load offset/scale in half type;
      size = quantized_sizes.at(file_idx);
      std::vector<half> host_array(size / sizeof(half));
      size_t loaded_data_size = size;
      in.seekg(0, in.end);
      in.seekg(0, in.beg);
      in.read((char *)host_array.data(), loaded_data_size);

      size_t in_get_size = in.gcount();
      if (in_get_size != loaded_data_size) {
        std::cout << "load weight data error " << in_get_size << ", "
                  << loaded_data_size << ", " << sizeof(half) << std::endl;
        return;
      }
      assert(size / sizeof(half) == host_array.size());
      // normal
      for (auto v : host_array) {
        *(half *)(ptr + data_index) = v;
        data_index += sizeof(half);
      }
    }
    in.close();
    file_idx++;
  }
}

void FileDataLoader::load_quantization_weight(FFModel *ff,
                                              Layer *l,
                                              int weight_idx,
                                              size_t volume,
                                              size_t num_replicas,
                                              char *weight,
                                              DataType data_type,
                                              Domain weight_domain) {
  size_t volume_ = 1;
  std::vector<int> dims_vec;
  for (int i = 0; i < weight_domain.get_dim(); i++) {
    int dim_i = weight_domain.hi()[i] - weight_domain.lo()[i] + 1;
    dims_vec.push_back(dim_i);
    volume_ *= dim_i;
  }
  assert(volume_ == volume * num_replicas);
  char *data = (char *)malloc(sizeof(char) * volume);

  std::string weight_filename = removeGuidOperatorName(std::string(l->name));

  if (weight_filename.find("attention") != std::string::npos &&
      weight_filename.rfind("attention") ==
          weight_filename.length() - strlen("attention")) {
    if (weight_idx == 0) {
      load_attention_weights_quantized(data,
                                       num_heads,
                                       hidden_dim,
                                       qkv_inner_dim,
                                       weight_filename,
                                       weights_folder,
                                       data_type,
                                       use_full_precision);
    }
    // else {
    //   load_attention_bias_quantized(data,
    //                                 num_heads,
    //                                 hidden_dim,
    //                                 qkv_inner_dim,
    //                                 weight_filename,
    //                                 weights_folder);
    // }

  } else {
    if (weight_idx > 0) {
      assert(weight_idx == 0 || weight_idx == 1);
      if (weight_filename != "embed_tokens_weight_lm_head") {
        weight_filename += weight_idx == 0 ? ".weight" : ".bias";
      }
    }
    load_from_quantized_file(data,
                             volume,
                             join_path({weights_folder, weight_filename}),
                             data_type,
                             use_full_precision);
  }

  char *ptr = weight;
  for (size_t i = 0; i < num_replicas; i++) {
    memcpy(ptr, data, volume * sizeof(char));
    ptr += volume;
  }

  free(data);
}

template <typename DT>
void FileDataLoader::load_single_weight_tensor(FFModel *ff,
                                               Layer *l,
                                               int weight_idx,
                                               size_t volume,
                                               size_t num_replicas,
                                               DT *weight,
                                               Domain weight_domain) {

  // Create a buffer to store weight data from the file
  size_t volume_ = 1;
  std::vector<int> dims_vec;
  for (int i = 0; i < weight_domain.get_dim(); i++) {
    int dim_i = weight_domain.hi()[i] - weight_domain.lo()[i] + 1;
    dims_vec.push_back(dim_i);
    volume_ *= dim_i;
  }
  assert(volume_ == volume * num_replicas);
  // assert(data_type_size(weight->data_type) == sizeof(DT));
  DT *data = (DT *)malloc(sizeof(DT) * volume);

  std::string weight_filename = removeGuidOperatorName(std::string(l->name));
  bool is_attn_proj = false, is_o_proj = false;

  // dense layers for attention projection is named as
  // self_attn.qkv_proj or self_attn.o_proj
  // so looking for self_attn. in the name can determine if it is an attention
  // projection
  if (weight_filename.find("attn.") != std::string::npos ||
      weight_filename.find("self_attention.") != std::string::npos) {
    size_t pos = weight_filename.find(".o_proj");
    if (pos != std::string::npos) {
      weight_filename.replace(pos, std::string(".o_proj").length(), "");
      is_o_proj = true;
    } else {
      pos = weight_filename.find(".qkv_proj");
      if (pos == std::string::npos) {
        cout << weight_filename << endl;
      }
      assert(pos != std::string::npos);
      weight_filename.replace(pos, std::string(".qkv_proj").length(), "");
    }
    is_attn_proj = true;
  }

  if (ff->config.benchmarking) {
    std::cout << "Initializing weight " << weight_filename
              << " with random data (benchmarking mode)" << std::endl;
    // If benchmarking, we don't need to load the weights
    // We can just fill the weight tensor with random data
  } else {
    if (l->op_type == OP_INC_MULTIHEAD_SELF_ATTENTION ||
        l->op_type == OP_SPEC_INC_MULTIHEAD_SELF_ATTENTION ||
        l->op_type == OP_TREE_INC_MULTIHEAD_SELF_ATTENTION) {
    } else if (is_attn_proj) {
      if (is_o_proj) {
        if (weight_idx == 0) {
          load_attention_weights_to_dense_v2(data,
                                             num_heads,
                                             num_kv_heads,
                                             hidden_dim,
                                             qkv_inner_dim,
                                             weight_filename,
                                             weights_folder,
                                             volume,
                                             tensor_parallelism_degree,
                                             true);
        } else {
          load_attention_o_proj_bias_to_dense_v2(data,
                                                 num_heads,
                                                 num_kv_heads,
                                                 hidden_dim,
                                                 qkv_inner_dim,
                                                 weight_filename,
                                                 weights_folder);
        }
      } else {
        if (weight_idx == 0) {
          load_attention_weights_to_dense_v2(data,
                                             num_heads,
                                             num_kv_heads,
                                             hidden_dim,
                                             qkv_inner_dim,
                                             weight_filename,
                                             weights_folder,
                                             volume,
                                             tensor_parallelism_degree,
                                             false);
        } else {
          load_attention_bias_v2(data,
                                 num_heads,
                                 num_kv_heads,
                                 hidden_dim,
                                 qkv_inner_dim,
                                 false, // do not load o_proj bias
                                 weight_filename,
                                 weights_folder,
                                 tensor_parallelism_degree);
        }
      }
    } else if (l->op_type == OP_ADD_BIAS_RESIDUAL_LAYERNORM) {
      assert(weight_idx >= 0 || weight_idx <= 2);
      weight_filename += (weight_idx == 0)
                             ? ".attn_bias"
                             : ((weight_idx == 1) ? ".weight" : ".bias");
      std::cout << "Loading weight file " << weight_filename << std::endl;
      std::string weight_filepath =
          join_path({weights_folder, weight_filename});
      load_from_file(data, volume, weight_filepath);
    } else {
      // default op
      assert(weight_idx == 0 || weight_idx == 1);
      // handle exception
      if (weight_filename != "embed_tokens_weight_lm_head") {
        weight_filename += weight_idx == 0 ? ".weight" : ".bias";
      }
      std::cout << "Loading weight file " << weight_filename << std::endl;
      std::string weight_filepath =
          join_path({weights_folder, weight_filename});
      load_from_file(data, volume, weight_filepath);
    }
  }

  // Copy the weight data from the buffer to the weight
  DT *ptr = weight;
  for (size_t i = 0; i < num_replicas; i++) {
    memcpy(ptr, data, volume * sizeof(DT));
    ptr += volume;
  }

  // Free buffer memory
  free(data);
}

void FileDataLoader::load_weight_task(
    Legion::Task const *task,
    std::vector<Legion::PhysicalRegion> const &regions,
    Legion::Context ctx,
    Legion::Runtime *runtime) {
  WeightLoadTaskArgs const *args = (WeightLoadTaskArgs const *)task->args;

  assert(task->regions.size() == regions.size());
  assert(regions.size() == 1); // one weight only
  GenericTensorAccessorW weight = helperGetGenericTensorAccessorWO(
      args->data_type, regions[0], task->regions[0], FID_DATA, ctx, runtime);
  Domain weight_domain = runtime->get_index_space_domain(
      ctx, task->regions[0].region.get_index_space());

  switch (args->data_type) {
    case DT_HALF: {
      args->loader->load_single_weight_tensor<half>(args->ff,
                                                    args->layer,
                                                    args->weight_idx,
                                                    args->volume,
                                                    args->num_replicas,
                                                    weight.get_half_ptr(),
                                                    weight_domain);
      break;
    }
    case DT_FLOAT: {
      args->loader->load_single_weight_tensor<float>(args->ff,
                                                     args->layer,
                                                     args->weight_idx,
                                                     args->volume,
                                                     args->num_replicas,
                                                     weight.get_float_ptr(),
                                                     weight_domain);
      break;
    }
    case DT_INT4:
    case DT_INT8: {
      args->loader->load_quantization_weight(args->ff,
                                             args->layer,
                                             args->weight_idx,
                                             args->volume,
                                             args->num_replicas,
                                             weight.get_byte_ptr(),
                                             args->data_type,
                                             weight_domain);
      break;
    }
    default:
      assert(false && "Unsupported data type");
  }
}

void FileDataLoader::load_weights_parallel(FFModel *ff,
                                           Context ctx,
                                           Runtime *runtime) {
  std::vector<Future> futures;

  for (Layer *l : ff->layers) {
    if (l->numWeights < 1 || l->name == NULL || strlen(l->name) < 1) {
      continue;
    }

    for (int i = 0; i < l->numWeights; i++) {
      Tensor weight = l->weights[i];
      if (weight == NULL) {
        continue;
      }

      if (weight->data_type != DT_FLOAT && weight->data_type != DT_HALF &&
          weight->data_type != DT_INT4 && weight->data_type != DT_INT8) {
        assert(false && "Unsupported data type");
      }

      ParallelTensor weight_pt;
      ff->get_parallel_tensor_from_tensor(weight, weight_pt);

      // Create task arguments
      size_t volume = 1, num_replicas = 1;
      if (weight_pt->sync_type == ParameterSyncType::NCCL) {
        for (int i = 0; i < weight_pt->num_dims; i++) {
          if (weight_pt->dims[i].is_replica_dim) {
            num_replicas *= weight_pt->dims[i].size;
          }
        }
      } else if (weight_pt->sync_type == ParameterSyncType::PS) {
        num_replicas = 1;
      } else {
        num_replicas = 1;
      }
      for (int i = 0; i < weight->num_dims; i++) {
        volume *= weight->dims[i];
      }
      WeightLoadTaskArgs args(
          ff, this, l, i, volume, num_replicas, weight->data_type);
      // launch task asynchronously
      TaskLauncher launcher(LOAD_WEIGHT_TASK_ID,
                            TaskArgument(&args, sizeof(WeightLoadTaskArgs)));
      launcher.add_region_requirement(RegionRequirement(
          weight_pt->region, WRITE_ONLY, EXCLUSIVE, weight_pt->region));
      launcher.add_field(0, FID_DATA);
      futures.push_back(runtime->execute_task(ctx, launcher));
    }
  }

  // Wait for all tasks to complete
  for (Future &f : futures) {
    f.get_void_result();
  }
}