// Copyright (C) 2021 THL A29 Limited, a Tencent company.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under
// the License.
//
// ╔════════════════════════════════════════════════════════════════════════════════════════╗
// ║──█████████╗───███████╗───████████╗───██╗──────██╗───███████╗───████████╗───████████╗───║
// ║──██╔══════╝──██╔════██╗──██╔════██╗──██║──────██║──██╔════██╗──██╔════██╗──██╔════██╗──║
// ║──████████╗───██║────██║──████████╔╝──██║──█╗──██║──█████████║──████████╔╝──██║────██║──║
// ║──██╔═════╝───██║────██║──██╔════██╗──██║█████╗██║──██╔════██║──██╔════██╗──██║────██║──║
// ║──██║─────────╚███████╔╝──██║────██║──╚████╔████╔╝──██║────██║──██║────██║──████████╔╝──║
// ║──╚═╝──────────╚══════╝───╚═╝────╚═╝───╚═══╝╚═══╝───╚═╝────╚═╝──╚═╝────╚═╝──╚═══════╝───║
// ╚════════════════════════════════════════════════════════════════════════════════════════╝
//
// Authors: Aster JIAN (asterjian@qq.com)
//          Yzx (yzxyzxyzx777@outlook.com)
//          Ao LI (346950981@qq.com)
//          Paul LU (lujq96@gmail.com)

#pragma once

#include <NvInfer.h>
#include <cuda_runtime.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_macros.h"
#include "common/trt_batch_stream.h"

FWD_NAMESPACE_BEGIN

// TrtInt8Calibrator should be set and used when INT8 infer mode is used. TrtEngine will internally
// use this calibrator to generator a calibration for INT8 quantization.
class TrtInt8Calibrator : public nvinfer1::IInt8LegacyCalibrator {
 public:
  // The calibrator uses the batch inputs generated byIBatchStream to feed the engine to get scale
  // factors, uses the algorithm to optain the scale factor of INT8 quantization, and stores scale
  // factors of layers in a cache file.
  // If the cache file exists, the calibrator will directly loaded scale factors from the given
  // cache file.
  TrtInt8Calibrator(std::shared_ptr<IBatchStream> stream, const std::string& cacheFilename,
                    const std::string& algo)
      : mStream(stream), mCalibrationTableName(cacheFilename) {
    assert(stream != nullptr);
    mAlgo = STR2CAT_MAPPING.find(algo)->second;
    mInputBytes = mStream->bytesPerBatch();
    mBatchSize = mStream->getBatchSize();
    for (auto& bytes : mInputBytes) {
      void* tmp;
      CUDA_CHECK(cudaMalloc(&tmp, mBatchSize * bytes));
      m_buffers.push_back(tmp);
    }
  }

  // The calibrator uses customized scale factors from a user-defined config file, uses the
  // algorithm to optimize the scale factor of INT8 quantization, and stores scale factors of
  // layers in a cache file.
  // If the cache file exists, the calibrator will directly loaded scale factors from the given
  // cache file.
  TrtInt8Calibrator(const std::string& cacheFilename, const std::string& algo, int batchsize,
                    float quantile = 0.9999)
      : mCalibrationTableName(cacheFilename), mBatchSize(batchsize), mQuantile(quantile) {
    mAlgo = STR2CAT_MAPPING.find(algo)->second;
  }

  // data buffers of batch input should be deleted when deconstructing.
  ~TrtInt8Calibrator() {
    if (m_buffers.size() != 0)
      for (auto& buffer : m_buffers) CUDA_CHECK(cudaFree(buffer));
  }

  virtual int getBatchSize() const { return mBatchSize; }

  // return true if next batch stream is fed to the corresponding binding inputs of the engine.
  // the batch_size of the batch data should equal to mBatchSize.
  virtual bool getBatch(void* bindings[], const char* names[], int nbBindings) {
    if (!mStream->next()) {
      return false;
    }
    auto batch_ptrs = mStream->getBatch();
    for (int i = 0; i < nbBindings; ++i) {
      CUDA_CHECK(cudaMemcpy(m_buffers[i], batch_ptrs[i], mInputBytes[i] * mBatchSize,
                            cudaMemcpyHostToDevice));
      bindings[i] = m_buffers[i];
    }
    return true;
  }

  // loading cache file of calibration
  virtual const void* readCalibrationCache(size_t& length) {
    std::ifstream fi(mCalibrationTableName, std::ios::binary);
    if (!fi) {
      length = 0;
      return nullptr;
    }
    fi.seekg(0, std::ios::end);
    length = fi.tellg();
    fi.seekg(0, std::ios::beg);
    mCalibrationCache.resize(length);
    fi.read(const_cast<char*>(mCalibrationCache.data()), length);
    fi.close();
    return mCalibrationCache.data();
  }

  // save calibration in a cache file
  virtual void writeCalibrationCache(const void* cache, size_t length) {
    std::ofstream fo(mCalibrationTableName, std::ios::binary);
    fo.write((const char*)cache, length);
    fo.close();
  }

  // load customized scale factors from a config file writtern by users
  virtual void setScaleFile(const std::string& filename) {
    union scaleField {
      uint32_t ieee754;
      float fp;
    };
    std::ifstream fi(filename);
    std::ofstream fo(mCalibrationTableName);
    if (!fi) {
      std::cout << "[ERROR ] Could not load user calibration scale file: " << filename << std::endl;
      return;
    }
    std::cout << "[INFO ] Reset calibration cache with scale file user provided: " << filename
              << std::endl;
    std::string line, tensorName, scaleStr;
    char delim = ':';
    while (std::getline(fi, line)) {
      std::istringstream iline(line);
      std::getline(iline, tensorName, delim);
      std::getline(iline, scaleStr, delim);
      if (scaleStr.size() == 0) {
        fo << tensorName << std::endl;
      } else {
        scaleField sf;
        sf.fp = std::stof(scaleStr) / 127.0f;
        fo << tensorName << ": " << std::hex << sf.ieee754 << std::endl;
        /* TensorRT scale cache to float.
        sf.ieee754 = std::strtoul(scaleStr.c_str(), NULL, 16);
        fo << tensorName << ":"<< sf.fp*127.0f << std::endl;*/
      }
    }
  }

  nvinfer1::CalibrationAlgoType getAlgorithm() { return mAlgo; }

  // Legacy Use Only
  double getQuantile() const { return mQuantile; }

  double getRegressionCutoff() const { return 1.0; }

  const void* readHistogramCache(std::size_t& length) { return nullptr; }

  void writeHistogramCache(const void* ptr, std::size_t length) { return; }

 protected:
  std::shared_ptr<IBatchStream> mStream;
  std::vector<void*> m_buffers;
  std::vector<int64_t> mInputBytes;
  double mQuantile;
  std::string mCalibrationCache{""};
  std::string mCalibrationTableName{""};
  nvinfer1::CalibrationAlgoType mAlgo{nvinfer1::CalibrationAlgoType::kENTROPY_CALIBRATION};
  int mBatchSize;
  const std::unordered_map<std::string, nvinfer1::CalibrationAlgoType> STR2CAT_MAPPING = {
      {"legacy", nvinfer1::CalibrationAlgoType::kLEGACY_CALIBRATION},        // 0
      {"entropy", nvinfer1::CalibrationAlgoType::kENTROPY_CALIBRATION},      // 1
      {"entropy_2", nvinfer1::CalibrationAlgoType::kENTROPY_CALIBRATION_2},  // 2
      {"minmax", nvinfer1::CalibrationAlgoType::kMINMAX_CALIBRATION},        // 3
  };
};

FWD_NAMESPACE_END