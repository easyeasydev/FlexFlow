# Copyright 2023 CMU, Facebook, LANL, MIT, NVIDIA, and Stanford (alphabetical)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from flexflow.serve.models import (
    FlexFlowLLAMA,
    FlexFlowOPT,
    FlexFlowFalcon,
    FlexFlowSTARCODER,
    FlexFlowMPT,
)
from flexflow.serve.models import (
    LLAMAConfig,
    OPTConfig,
    FalconConfig,
    STARCODERConfig,
    MPTConfig,
)
from flexflow.core import *
from transformers import AutoConfig, AutoModelForCausalLM, AutoTokenizer
from peft import PeftModel, PeftConfig, LoraConfig
from huggingface_hub import HfApi
import torch, shutil, hashlib, json, gc
from typing import Union, List, Tuple
from safetensors import safe_open
from huggingface_hub import snapshot_download

from enum import Enum


class CachedResourceType(Enum):
    TOKENIZER = "tokenizer"
    WEIGHTS = "weights"


class _SupportedModels:
    def __init__(
        self,
    ):
        self.supported_models = {
            "LlamaForCausalLM": (ModelType.LLAMA, FlexFlowLLAMA, LLAMAConfig),
            "LLaMAForCausalLM": (ModelType.LLAMA, FlexFlowLLAMA, LLAMAConfig),
            "OPTForCausalLM": (ModelType.OPT, FlexFlowOPT, OPTConfig),
            "RWForCausalLM": (ModelType.FALCON, FlexFlowFalcon, FalconConfig),
            "FalconForCausalLM": (ModelType.FALCON, FlexFlowFalcon, FalconConfig),
            "GPTBigCodeForCausalLM": (
                ModelType.STARCODER,
                FlexFlowSTARCODER,
                STARCODERConfig,
            ),
            "MPTForCausalLM": (ModelType.MPT, FlexFlowMPT, MPTConfig),
        }

    def get_ff_model_type(self, hf_config):
        architectures = getattr(hf_config, "architectures", [])
        ff_arch = None
        if next(iter(architectures), None) is not None:
            ff_arch = self.supported_models.get(architectures[0])
        if ff_arch is None:
            raise ValueError(
                f"Huggingface model of type {architectures} is not yet supported by FlexFlow"
            )
        return ff_arch


class LLM:
    """This class creates a LLM (Large-Language Model) object based on a model from HuggingFace"""

    def __init__(
        self,
        model_name: str,
        data_type: DataType = DataType.DT_HALF,
        cache_path: str = "",
        refresh_cache: bool = False,
        output_file: str = "",
    ):
        """Create the LLM object

        :param model_name: The name of the HuggingFace model to use. E.g. 'meta-llama/Llama-2-7b-hf'
        :type model_name: str
        :param data_type: The data type to use for the tensors (e.g. DataType.DT_FLOAT for full precision, or DataType.DT_HALF for half precision), defaults to DataType.DT_HALF
        :type data_type: DataType, optional
        :param cache_path: Path to the folder (which will be created if it does not yet exist) to use for the FlexFlow weights/tokenizers cache, defaults to "~/.cache/flexflow"
        :type tokenizer_path: str, optional
        :param refresh_cache: Use this flag to force the refresh of the model's weights/tokenizer cache, defaults to False
        :type refresh_cache: bool, optional
        :param output_file: Path to the output file. If left blank, the output will not be written to file, defaults to ""
        :type output_file: str, optional
        """
        self.supported_models = _SupportedModels()
        self.hf_config = AutoConfig.from_pretrained(model_name, trust_remote_code=True)
        self.model_name = self.hf_config._name_or_path
        (
            self.model_type,
            self.model_class,
            self.config_class,
        ) = self.supported_models.get_ff_model_type(self.hf_config)
        self.data_type = data_type
        assert self.data_type == DataType.DT_HALF or self.data_type == DataType.DT_FLOAT
        self.cache_path = cache_path if len(cache_path) > 0 else "~/.cache/flexflow"
        self.refresh_cache = refresh_cache
        self.output_file = output_file
        self.rm = None
        self.pefts = {}
        self.tokenizer = None

    def __del__(self):
        # Stop the background server before deleting the object
        if type(self) == LLM and self.rm is not None:
            self.rm.stop_server()

    def register_peft_adapter(self, lora_config: LoraLinearConfig):
        """Add a PEFT adapter to the LLM"""
        if lora_config is None:
            raise ValueError("lora_config cannot be None")
        if len(lora_config.peft_model_id or "") == 0:
            raise ValueError("PEFT model id cannot be empty")
        # Inference (trainable=False): LoRA model should already exist in huggingface. Any changes of parameters from original model are ignored
        # Training (trainable=True): Either an existing model (init_lora_weights=False) or a new one (init_lora_weights=True)

        if lora_config.trainable == False or not lora_config.init_lora_weights:
            peft_config = PeftConfig.from_pretrained(lora_config.peft_model_id)
        else:
            peft_config = LoraConfig(
                peft_type="LORA",
                base_model_name_or_path=self.model_name,
                r=lora_config.rank,
                target_modules=lora_config.target_modules,
                lora_alpha=lora_config.lora_alpha,
                lora_dropout=lora_config.lora_dropout,
                init_lora_weights=lora_config.init_lora_weights,
            )
        if peft_config.peft_type != "LORA":
            raise RuntimeError(
                f"PEFT type {peft_config.peft_type} not yet supported in FlexFlow"
            )
        if "base_model_name_or_path" not in peft_config.to_dict():
            raise ValueError(
                f"PEFT model {lora_config.peft_model_id} does not have an associated base model"
            )
        if peft_config.base_model_name_or_path != self.model_name:
            raise RuntimeError(
                f"Attempting to add PEFT with base model name {peft_config.base_model_name_or_path} to LLM {self.model_name}"
            )

        lora_config.ff_compile()

        self.pefts[lora_config] = {
            "peft_config": peft_config,
            "peft_type": peft_config.peft_type,
            "ff_peft_model_id": self.model.ffmodel.register_peft_adapter(lora_config),
        }

    def get_ff_peft_id(self, lora_config: LoraLinearConfig) -> PEFTModelID:
        if lora_config is None:
            raise ValueError("lora_config cannot be None")
        if len(lora_config.peft_model_id or "") == 0:
            raise ValueError("PEFT model id cannot be empty")
        if lora_config not in self.pefts:
            raise ValueError(
                f"PEFT {lora_config} not registered with LLM {self.model_name}"
            )
        if "ff_peft_model_id" not in self.pefts[lora_config]:
            raise RuntimeError(
                f"Attempting to run PEFT {lora_config} before compiling LLM {self.model_name}"
            )

        return self.pefts[lora_config]["ff_peft_model_id"]

    def download_hf_config(self):
        """Save the HuggingFace model configs to a json file. Useful mainly to run the C++ inference code."""
        config_dir = os.path.join(
            os.path.expanduser(self.cache_path), "configs", self.model_name.lower()
        )
        config_path = os.path.join(config_dir, "config.json")
        os.makedirs(config_dir, exist_ok=True)
        print(f"Creating directory {config_dir} (if it doesn't exist)...")
        print(f"Saving {self.model_name} configs to file {config_path}...")
        # self.hf_config.to_json_file(config_path)
        src_folder = snapshot_download(
            repo_id=self.model_name, allow_patterns="config.json"
        )
        src_path = os.path.join(src_folder, "config.json")
        if os.path.exists(src_path):
            shutil.copy(src_path, config_path)

    def __get_revision_hashes(
        self, model_name: str, folder: str
    ) -> Tuple[Union[str, None], str, str]:
        """Return the commit hash of the object (weight, tokenizer, etc) cached by FlexFlow and the latest commit hash of the object from HuggingFace (or other source)

        Args:
            model_name (str): Name of the model cached by FlexFlow
            folder (str): Folder where the cached object is stored

        Returns:
            ff_revision: Commit hash of the object cached by FlexFlow
            ff_revision_filepath: Path to the file containing the commit hash of the object cached by FlexFlow
            latest_revision: Latest commit hash of the object from HuggingFace (or other source)
        """
        ff_revision = None
        ff_revision_filepath = os.path.join(folder, "rev_sha.txt")

        if os.path.exists(ff_revision_filepath):
            ff_revision = "".join(open(ff_revision_filepath).read().split())

        if os.path.exists(model_name) and os.path.isdir(model_name):
            # Local model
            files = os.listdir(model_name)
            state = files + [
                os.path.getmtime(os.path.join(model_name, f)) for f in files
            ]
            latest_revision = hashlib.md5(str(state).encode("utf-8")).hexdigest()
        else:
            # Remote HuggingFace model
            hf_api = HfApi()
            latest_revision = hf_api.model_info(self.model_name).sha
        return ff_revision, latest_revision

    def __get_resource_path(
        self, model_name: str, resource_type: CachedResourceType
    ) -> str:
        """Returns the path to the folder where the model weights or tokenizer files are stored

        Args:
            model_name (str): Name of the model
            resource_type (CachedResourceType): Whether to get the path to the weights or the tokenizer

        Returns:
            str: Path to the folder where the model weights or tokenizer files are stored
        """
        if resource_type == CachedResourceType.WEIGHTS:
            return os.path.join(
                os.path.expanduser(self.cache_path),
                "weights",
                model_name.lower(),
                (
                    "full-precision"
                    if self.data_type == DataType.DT_FLOAT
                    else "half-precision"
                ),
            )
        elif resource_type == CachedResourceType.TOKENIZER:
            return os.path.join(
                os.path.expanduser(self.cache_path), "tokenizers", model_name.lower()
            )
        else:
            raise ValueError(f"Invalid resource type {resource_type}")

    def __need_cache_refresh(
        self, model_name: str, resource_type: CachedResourceType
    ) -> bool:
        """Check whether the model weights or tokenizer files are available and up to date.
        If they need a refresh, create the folder for the resource, save the new commit hash to the rev_sha.txt file, delete any existing files, and return true.

        Args:
            model_name (str): Name of the model to check
            resource_type (CachedResourceType): Whether to check the weights or the tokenizer

        Returns:
            bool: True if the weights or tokenizer need a refresh, False otherwise
        """
        resource_path = self.__get_resource_path(model_name, resource_type)
        ff_revision, latest_revision = self.__get_revision_hashes(self.model_name, resource_path)
        if self.refresh_cache or not os.path.exists(resource_path) or ff_revision != latest_revision:
            print(
                f"Refreshing {resource_type} in cache for model {model_name} at path {resource_path} ..."
            )
            if os.path.exists(resource_path):
                shutil.rmtree(resource_path)
            os.makedirs(resource_path, exist_ok=True)
            ff_revision_file = os.path.join(resource_path, "rev_sha.txt")
            with open(ff_revision_file, "w+") as f:
                f.write(latest_revision)
            return True
        return False

    def download_hf_weights_if_needed(self) -> None:
        """Check in the folder specified by the cache_path whether the LLM's model weights are available and up to date.
        If not, or if the refresh_cache parameter is set to True, download new weights and convert them.
        """

        # TODO: edit this to download the weights using snapshot_download and convert them to FlexFlow format without loading them to GPU
        def download_and_convert_llm_weights(model_name):
            hf_model = AutoModelForCausalLM.from_pretrained(
                model_name,
                trust_remote_code=True,
                torch_dtype=(
                    torch.float32
                    if self.data_type == DataType.DT_FLOAT
                    else torch.float16
                ),
            )
            # Convert the model to FlexFlow format
            weights_path = self.__get_resource_path(
                model_name, CachedResourceType.WEIGHTS
            )
            self.model_class.convert_hf_model(hf_model, weights_path)
            # Save new revision hash to file
            print(f"Done converting the weights for model {self.model_name}")
            # Deallocate hf model
            del hf_model
            gc.collect()
            torch.cuda.empty_cache()

        need_refresh = self.__need_cache_refresh(
            self.model_name, CachedResourceType.WEIGHTS
        )
        if need_refresh:
            print(
                f"'{self.model_name}' local model weights need updating! Downloading/converting new weights now..."
            )
            download_and_convert_llm_weights(self.model_name)

    def download_hf_tokenizer_if_needed(self):
        """Check in the folder specified by the cache_path whether the LLM's tokenizer files are available and up to date.
        If not, or if the refresh_cache parameter is set to True, download new tokenizer files.
        """
        print("Loading tokenizer...")

        # Use local cache, or download new version
        need_refresh = self.__need_cache_refresh(
            self.model_name, CachedResourceType.TOKENIZER
        )
        if need_refresh:
            print(
                f"'{self.model_name}' tokenizer needs updating! Downloading tokenizer now..."
            )
            # Load/download the tokenizer files
            target_tokenizer_files = [
                "tokenizer.json",
                "tokenizer_config.json",
                "special_tokens_map.json",
                "vocab.json",
                "merges.txt",
            ]
            if os.path.exists(self.model_name):
                hf_tokenizer_path = self.model_name
            else:
                hf_tokenizer_path = snapshot_download(
                    repo_id=self.model_name, allow_patterns=target_tokenizer_files
                )
            tokenizer_path = self.__get_resource_path(
                self.model_name, CachedResourceType.TOKENIZER
            )
            for file in target_tokenizer_files:
                src_path = os.path.join(hf_tokenizer_path, file)
                dst_path = os.path.join(tokenizer_path, file)
                if os.path.exists(src_path):
                    shutil.copy(src_path, dst_path)
            print("Done updating HF tokenizer.")

    def download_peft_adapter_if_needed(self, hf_peft_model_id: str):
        """Check in the folder specified by the cache_path whether the PEFT model weights are available and up to date.
        If not, or if the refresh_cache parameter is set to True, download new weights and convert them.
        """

        def download_and_convert_peft_model(hf_peft_model_id: str):
            if (
                self.data_type != DataType.DT_FLOAT
                and self.data_type != DataType.DT_HALF
            ):
                raise ValueError(
                    "data_type must be either DataType.DT_FLOAT or DataType.DT_HALF"
                )

            # Save peft config to file
            peft_config_dir = os.path.join(
                os.path.expanduser(self.cache_path), "configs", hf_peft_model_id.lower()
            )
            dst_path = os.path.join(peft_config_dir, "config.json")
            os.makedirs(peft_config_dir, exist_ok=True)
            print(f"Saving {hf_peft_model_id} configs to file {dst_path}...")
            config_path = snapshot_download(
                repo_id=hf_peft_model_id, allow_patterns="adapter_config.json"
            )
            src_path = os.path.join(config_path, "adapter_config.json")
            if os.path.exists(src_path):
                shutil.copy(src_path, dst_path)

            # Save peft weights to file
            adapter_path = snapshot_download(
                repo_id=hf_peft_model_id, allow_patterns="adapter_model.safetensors"
            )
            weights_path = self.__get_resource_path(
                hf_peft_model_id.lower(), CachedResourceType.WEIGHTS
            )
            with safe_open(adapter_path, framework="pt", device="cpu") as f:
                for tensor_name in f.keys():
                    tensor = f.get_tensor(tensor_name)
                    if self.data_type == DataType.DT_HALF:
                        tensor = tensor.half()
                    else:
                        tensor = tensor.float()
                    tensor_name = tensor_name.replace(
                        "base_model.model.model.", ""
                    ).replace(".default", "")
                    print(tensor_name)

                    tensor_name = self.model_class.convert_hf_weight_name(tensor_name)
                    tensor.detach().cpu().numpy().tofile(
                        f"{weights_path}/{tensor_name}"
                    )

        need_refresh = self.__need_cache_refresh(
            hf_peft_model_id, CachedResourceType.WEIGHTS
        )
        if need_refresh:
            print(
                f"'{hf_peft_model_id}' local model weights need updating! Downloading/converting new weights now..."
            )
            download_and_convert_peft_model(hf_peft_model_id)

    def compile(
        self,
        generation_config: GenerationConfig = GenerationConfig(),
        max_requests_per_batch: int = 1,
        max_seq_length: int = 256,
        max_tokens_per_batch: int = 64,
        max_concurrent_adapters: int = 1,
        enable_peft_finetuning: bool = False,
        ssms: list = [],
    ):
        """Compile the LLM for inference and load the weights into memory

        :param generation_config: The GenerationConfig object with the configurations to use for sampling, defaults to GenerationConfig()
        :type generation_config: GenerationConfig, optional
        :param max_requests_per_batch: The maximum batch size to allow, defaults to 1
        :type max_requests_per_batch: int, optional
        :param max_seq_length: The maximum sequence length to allow per batch, defaults to 256
        :type max_seq_length: int, optional
        :param max_tokens_per_batch: The maximum number of tokens (across requests) to allow per batch, defaults to 64
        :type max_tokens_per_batch: int, optional
        :param max_concurrent_adapters: The maximum number of concurrent LoRA adapters, defaults to 1
        :type max_concurrent_adapters: int, optional
        :param enable_peft_finetuning: Whether to enable support for PEFT fine-tuning, defaults to False
        :type enable_peft_finetuning: bool, optional
        :param ssms: The SSMs to use when operating in speculative inference mode, defaults to []
        :type ssms: list, optional
        """
        self.ssms = ssms
        self.generation_config = GenerationConfig()
        self.ffconfig = FFConfig()
        if len(ssms) > 0:
            assert type(self) == LLM
            mode = InferenceMode.TREE_VERIFY_MODE
        elif type(self) == SSM:
            mode = InferenceMode.BEAM_SEARCH_MODE
            self.ffconfig.data_parallelism_degree = 1
            self.ffconfig.tensor_parallelism_degree = 1
            self.ffconfig.pipeline_parallelism_degree = 1
        else:
            assert type(self) == LLM
            mode = InferenceMode.INC_DECODING_MODE

        self.max_seq_length = max_seq_length

        # Create request manager and set serving configuration
        self.rm = RequestManager()
        self.rm.set_max_requests_per_batch(max_requests_per_batch)
        self.rm.set_max_tokens_per_batch(max_tokens_per_batch)
        self.rm.set_max_sequence_length(max_seq_length)
        self.rm.set_max_concurrent_adapters(max_concurrent_adapters)
        self.rm.set_enable_peft_finetuning(enable_peft_finetuning)

        # Instantiate the relevant model
        self.model = self.model_class(
            mode,
            generation_config,
            self.ffconfig,
            self.hf_config,
            self.data_type,
            max_tokens_per_batch,
        )

        # Download the config from huggingface
        self.download_hf_config()

        # Download the tokenizer from huggingface (if needed) and load them
        self.download_hf_tokenizer_if_needed()

        # Download the weights from huggingface (if needed)
        self.download_hf_weights_if_needed()

        # Create file data loader, load weights into tensors
        model_configs = self.config_class(self.hf_config)

        self.rm.set_max_spec_tree_token_num(
            model_configs.max_spec_tree_token_num
            if "max_spec_tree_token_num" in model_configs.__dict__
            else 20
        )

        weights_path = self.__get_resource_path(
            self.model_name, CachedResourceType.WEIGHTS
        )
        self.fileloader = FileDataLoader(
            weights_path,
            model_configs.num_attention_heads,
            model_configs.num_key_value_heads,
            model_configs.hidden_size,
            model_configs.hidden_size // model_configs.num_attention_heads,
            self.ffconfig.tensor_parallelism_degree,
            self.data_type == DataType.DT_FLOAT,
        )

        # Register weights file loader
        self.im = InferenceManager()
        self.im.register_model_weights_loader(self.model.ffmodel, self.fileloader)

        # Create tokenizer (this must be done after we have downloaded the tokenizer
        bos_token_id = (
            -1 if self.hf_config.bos_token_id is None else self.hf_config.bos_token_id
        )
        eos_token_id = (
            -1 if self.hf_config.eos_token_id is None else self.hf_config.eos_token_id
        )
        if type(eos_token_id) == int:
            eos_token_id = [eos_token_id]
        elif type(eos_token_id) != list:
            raise ValueError("eos_token_id must be an integer or a list of integers")
        tokenizer_path = self.__get_resource_path(
            self.model_name, CachedResourceType.TOKENIZER
        )
        self.rm.register_tokenizer(
            self.model_type, bos_token_id, eos_token_id, tokenizer_path
        )
        self.rm.register_output_filepath(self.output_file)

        for ssm in self.ssms:
            self.rm.register_ssm_model(ssm.model.ffmodel)

        # start background server
        if (mode == InferenceMode.TREE_VERIFY_MODE) or (
            mode == InferenceMode.INC_DECODING_MODE
        ):
            import atexit

            atexit.register(self.rm.stop_server)

    def _generate(self, requests: List[Request]) -> List[GenerationResult]:
        if len(requests) == 0:
            return []
        for req in requests:
            if req.req_type == RequestType.REQ_INFERENCE:
                # check max_length and max_new_tokens parameters
                if req.max_length == -1 and req.max_new_tokens == -1:
                    req.max_length = self.max_seq_length - 1
                elif req.max_length != -1 and req.max_new_tokens != -1:
                    warnings.warn(
                        f"Both `max_new_tokens` (={req.max_new_tokens}) and `max_length`(={req.max_length}) seem to have been set. `max_new_tokens` will take precedence."
                    )
                    req.max_length = -1
                if (
                    req.max_length >= self.max_seq_length
                    or req.max_new_tokens >= self.max_seq_length
                ):
                    raise ValueError(
                        f"max_length ({req.max_length}) or max_new_tokens ({req.max_new_tokens}) exceeds the maximum sequence length ({self.max_seq_length})"
                    )
            else:
                if req.max_new_tokens != -1:
                    raise ValueError(
                        f"max_new_tokens ({req.max_new_tokens}) is not allowed for finetuning requests."
                    )
                if req.max_length == -1:
                    req.max_length = self.max_seq_length - 1
                if req.max_length >= self.max_seq_length:
                    raise ValueError(
                        f"max_length ({req.max_length}) exceeds the maximum sequence length ({self.max_seq_length})"
                    )
        return self.model.ffmodel.generate(requests)

    def __chat2prompt(self, messages: List[dict]) -> str:
        """Convert a list of messages to a single prompt string

        :param messages: The list of messages to convert
        :type messages: List[dict]
        :return: The prompt string
        :rtype: str
        """
        # ensure that each element is a dictionary, containing the "role" and "content" keys
        for message in messages:
            if (
                type(message) != dict
                or "role" not in message
                or "content" not in message
            ):
                raise ValueError(
                    "Each element in the list must be a dictionary with the keys 'role' and 'content'"
                )
        if self.tokenizer is None:
            self.tokenizer = AutoTokenizer.from_pretrained(self.model_name)
        if self.tokenizer.chat_template is None:
            raise ValueError(
                f"Model {self.model_name} does not support chat completion"
            )
        return self.tokenizer.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True
        )

    def __output2chat_response(
        self, requests: List[Request], outputs: List[GenerationResult]
    ) -> List[GenerationResult]:
        assert len(requests) == len(outputs)
        for i in range(len(outputs)):
            outputs[i].output_text = outputs[i].output_text[len(requests[i].prompt) :]
        return outputs

    def generate(
        self,
        requests_or_prompts: Union[str, List[str], List[dict], Request, List[Request]],
        max_length: int = -1,
        max_new_tokens: int = -1,
    ):
        """Generate tokens based on the input prompt(s)

        :param requests_or_prompts: The generation prompt(s) in the form of a string, a list of strings, a Request, or list of Requests
        :type requests_or_prompts: Union[str, List[str], Request, List[Request]]
        :param max_length: The maximum length in tokens of the prompt + generated sequence, defaults to -1 (no maximum length)
        :type max_length: int, optional
        :param max_new_tokens: The maximum number of new tokens (excluding the prompt) to generate, defaults to 128
        :type max_new_tokens: int, optional
        :return: the generation results
        :rtype: GenerationResult
        """
        if type(requests_or_prompts) == str:
            if len(requests_or_prompts) == 0:
                return []
            request = Request(
                req_type=RequestType.REQ_INFERENCE,
                prompt=requests_or_prompts,
                max_length=max_length,
                max_new_tokens=max_new_tokens,
            )
            return self._generate([request])
        elif type(requests_or_prompts) == Request:
            return self._generate([requests_or_prompts])
        elif type(requests_or_prompts) == list:
            if len(requests_or_prompts) == 0:
                return []
            if type(requests_or_prompts[0]) == str:
                requests = [
                    Request(
                        req_type=RequestType.REQ_INFERENCE,
                        prompt=req,
                        max_length=max_length,
                        max_new_tokens=max_new_tokens,
                    )
                    for req in requests_or_prompts
                ]
                return self._generate(requests)
            elif type(requests_or_prompts[0]) == dict:
                prompt = self.__chat2prompt(requests_or_prompts)
                request = Request(
                    req_type=RequestType.REQ_INFERENCE,
                    prompt=prompt,
                    max_length=max_length,
                    max_new_tokens=max_new_tokens,
                    add_special_tokens=False,
                )
                outputs = self._generate([request])
                return self.__output2chat_response([request], outputs)
            elif type(requests_or_prompts[0]) == list:
                prompts = [
                    self.__chat2prompt(messages) for messages in requests_or_prompts
                ]
                requests = [
                    Request(
                        req_type=RequestType.REQ_INFERENCE,
                        prompt=prompt,
                        max_length=max_length,
                        max_new_tokens=max_new_tokens,
                        add_special_tokens=False,
                    )
                    for prompt in prompts
                ]
                outputs = self._generate(requests)
                return self.__output2chat_response(requests, outputs)
            elif type(requests_or_prompts[0]) == Request:
                print(requests_or_prompts)
                return self._generate(requests_or_prompts)
        else:
            assert (
                False
            ), "Please pass a string, list of strings, Request, or list of Requests"

    def start_server(self):
        self.rm.start_server(self.model.ffmodel)
        print("Background server started.")

    def stop_server(self):
        self.rm.stop_server()
        print("Background server stopped.")


class SSM(LLM):
    """This class creates a SSM (Small-Speculative Model) object based on a model from HuggingFace"""

    def __init__(
        self,
        model_name: str,
        data_type: DataType = DataType.DT_HALF,
        cache_path: str = "~/.cache/flexflow",
        refresh_cache: bool = False,
        output_file: str = "",
    ):
        """Create the SSM object

        :param model_name: The name of the HuggingFace model to use. E.g. 'meta-llama/Llama-2-7b-hf'
        :type model_name: str
        :param data_type: The data type to use for the tensors (e.g. DataType.DT_FLOAT for full precision, or DataType.DT_HALF for half precision), defaults to DataType.DT_HALF
        :type data_type: DataType, optional
        :param cache_path: Path to the folder (which will be created if it does not yet exist) to use for the FlexFlow weights/tokenizers cache, defaults to "~/.cache/flexflow"
        :type tokenizer_path: str, optional
        :param refresh_cache: Use this flag to force the refresh of the model's weights/tokenizer cache, defaults to False
        :type refresh_cache: bool, optional
        :param output_file: Path to the output file. If left blank, the output will not be written to file, defaults to ""
        :type output_file: str, optional
        """
        super().__init__(model_name, data_type, cache_path, refresh_cache, output_file)

    def compile(
        self,
        generation_config: GenerationConfig = GenerationConfig(),
        max_requests_per_batch: int = 16,
        max_seq_length: int = 256,
        max_tokens_per_batch: int = 2048,
        max_concurrent_adapters: int = 1,
        enable_peft_finetuning: bool = False,
        ssms: list = [],
    ):
        """Compile the SSM for inference and load the weights into memory
        :param generation_config: The GenerationConfig object with the configurations to use for sampling, defaults to GenerationConfig()
        :type generation_config: GenerationConfig, optional
        :param max_requests_per_batch: The maximum batch size to allow, defaults to 16
        :type max_requests_per_batch: int, optional
        :param max_seq_length: The maximum sequence length to allow per batch, defaults to 256
        :type max_seq_length: int, optional
        :param max_tokens_per_batch: The maximum number of tokens (across requests) to allow per batch, defaults to 2048
        :type max_tokens_per_batch: int, optional
        :param max_concurrent_adapters: The maximum number of concurrent LoRA adapters, defaults to 1
        :type max_concurrent_adapters: int, optional
        :param enable_peft_finetuning: Whether to enable support for PEFT fine-tuning, defaults to False
        :type enable_peft_finetuning: bool, optional
        :param ssms: The SSMs to use when operating in speculative inference mode, defaults to []
        :type ssms: list, optional
        """
        super().compile(
            generation_config,
            max_requests_per_batch,
            max_seq_length,
            max_tokens_per_batch,
            max_concurrent_adapters,
            enable_peft_finetuning,
            ssms,
        )
