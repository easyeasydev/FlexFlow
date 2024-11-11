#include "models/dlrm/dlrm.h"
#include "pcg/computation_graph.h"
#include "utils/containers/concat_vectors.h"
#include "utils/containers/transform.h"
#include "utils/containers/zip.h"

namespace FlexFlow {

/**
 * @brief Get the default DLRM config.
 *
 * @details The configs here refer to the example at
 * https://github.com/flexflow/FlexFlow/blob/inference/examples/cpp/DLRM/dlrm.cc.
 */
DLRMConfig get_default_dlrm_config() {
  return DLRMConfig{
      /*embedding_dim=*/64,
      /*embedding_bag_size=*/1,
      /*embedding_size=*/
      std::vector<int>{
          1000000,
          1000000,
          1000000,
          1000000,
      },
      /*dense_arch_layer_sizes=*/
      std::vector<size_t>{
          4,
          64,
          64,
      },
      /*over_arch_layer_sizes=*/
      std::vector<size_t>{
          64,
          64,
          2,
      },
      /*arch_interaction_op=*/"cat",
      /*batch_size=*/64,
      /*seed=*/std::rand(),
  };
}

tensor_guid_t create_dlrm_mlp(ComputationGraphBuilder &cgb,
                              DLRMConfig const &config,
                              tensor_guid_t const &input,
                              std::vector<size_t> const &mlp_layers) {
  tensor_guid_t t = input;
  for (size_t i = 0; i < mlp_layers.size() - 1; i++) {
    float std_dev = sqrt(2.0f / (mlp_layers[i + 1] + mlp_layers[i]));
    InitializerAttrs projection_initializer =
        InitializerAttrs{NormInitializerAttrs{
            /*seed=*/config.seed,
            /*mean=*/0,
            /*stddev=*/std_dev,
        }};

    std_dev = sqrt(2.0f / mlp_layers[i + 1]);
    InitializerAttrs bias_initializer = InitializerAttrs{NormInitializerAttrs{
        /*seed=*/config.seed,
        /*mean=*/0,
        /*stddev=*/std_dev,
    }};

    t = cgb.dense(/*input=*/t,
                  /*outDim=*/mlp_layers[i + 1],
                  /*activation=*/Activation::RELU,
                  /*use_bias=*/true,
                  /*data_type=*/DataType::FLOAT,
                  /*projection_initializer=*/projection_initializer,
                  /*bias_initializer=*/bias_initializer);
  }
  return t;
}

tensor_guid_t create_dlrm_sparse_embedding_network(ComputationGraphBuilder &cgb,
                                                   DLRMConfig const &config,
                                                   tensor_guid_t const &input,
                                                   int input_dim,
                                                   int output_dim) {
  float range = sqrt(1.0f / input_dim);
  InitializerAttrs embed_initializer = InitializerAttrs{UniformInitializerAttrs{
      /*seed=*/config.seed,
      /*min_val=*/-range,
      /*max_val=*/range,
  }};

  tensor_guid_t t = cgb.embedding(input,
                                  /*num_entries=*/input_dim,
                                  /*outDim=*/output_dim,
                                  /*aggr=*/AggregateOp::SUM,
                                  /*dtype=*/DataType::HALF,
                                  /*kernel_initializer=*/embed_initializer);
  return cgb.cast(t, DataType::FLOAT);
}

tensor_guid_t create_dlrm_interact_features(
    ComputationGraphBuilder &cgb,
    DLRMConfig const &config,
    tensor_guid_t const &bottom_mlp_output,
    std::vector<tensor_guid_t> const &emb_outputs) {
  if (config.arch_interaction_op != "cat") {
    throw mk_runtime_error(fmt::format(
        "Currently only arch_interaction_op=cat is supported, but found "
        "arch_interaction_op={}. If you need support for additional "
        "arch_interaction_op value, please create an issue.",
        config.arch_interaction_op));
  }

  return cgb.concat(
      /*tensors=*/concat_vectors({bottom_mlp_output}, emb_outputs),
      /*axis=*/1);
}

ComputationGraph get_dlrm_computation_graph(DLRMConfig const &config) {
  ComputationGraphBuilder cgb;

  auto create_input_tensor = [&](FFOrdered<size_t> const &dims,
                                 DataType const &data_type) -> tensor_guid_t {
    TensorShape input_shape = TensorShape{
        TensorDims{dims},
        data_type,
    };
    return cgb.create_input(input_shape, CreateGrad::YES);
  };

  // Create input tensors
  std::vector<tensor_guid_t> sparse_inputs(
      config.embedding_size.size(),
      create_input_tensor({config.batch_size, config.embedding_bag_size},
                          DataType::INT64));

  tensor_guid_t dense_input = create_input_tensor(
      {config.batch_size, config.dense_arch_layer_sizes.front()},
      DataType::FLOAT);

  // Construct the model
  tensor_guid_t bottom_mlp_output = create_dlrm_mlp(
      /*cgb=*/cgb,
      /*config=*/config,
      /*input=*/dense_input,
      /*mlp_layers=*/config.dense_arch_layer_sizes);

  std::vector<tensor_guid_t> emb_outputs;
  for (size_t i = 0; i < config.embedding_size.size(); i++) {
    int input_dim = config.embedding_size.at(i);
    emb_outputs.emplace_back(create_dlrm_sparse_embedding_network(
        /*cgb=*/cgb,
        /*config=*/config,
        /*input=*/sparse_inputs.at(i),
        /*input_dim=*/input_dim,
        /*output_dim=*/config.embedding_dim));
  }

  tensor_guid_t interacted_features = create_dlrm_interact_features(
      /*cgb=*/cgb,
      /*config=*/config,
      /*bottom_mlp_output=*/bottom_mlp_output,
      /*emb_outputs=*/emb_outputs);

  tensor_guid_t output = create_dlrm_mlp(
      /*cgb=*/cgb,
      /*config=*/config,
      /*input=*/interacted_features,
      /*mlp_layers=*/config.over_arch_layer_sizes);

  return cgb.computation_graph;
}

} // namespace FlexFlow
