#include "depth_anything.h"

#include <NvOnnxParser.h>

#define isFP16 true

using namespace nvinfer1;

/**
 * @brief DepthAnything`s constructor
 * @param model_path DepthAnything engine file path
 * @param logger Nvinfer ILogger
 */
DepthAnything::DepthAnything() {}

void DepthAnything::init(std::string model_path, nvinfer1::ILogger& logger) {
  // Deserialize an engine
  if (model_path.find(".onnx") == std::string::npos) {
    // read the engine file
    std::ifstream engineStream(model_path, std::ios::binary);
    engineStream.seekg(0, std::ios::end);
    const size_t modelSize = engineStream.tellg();
    engineStream.seekg(0, std::ios::beg);
    std::unique_ptr<char[]> engineData(new char[modelSize]);
    engineStream.read(engineData.get(), modelSize);
    engineStream.close();

    // create tensorrt model
    runtime = nvinfer1::createInferRuntime(logger);
    engine = runtime->deserializeCudaEngine(engineData.get(), modelSize);
    context = engine->createExecutionContext();

  }
  // Build an engine from an onnx model
  else {
    build(model_path, logger);
    saveEngine(model_path);
  }

#if NV_TENSORRT_MAJOR < 10
  // Define input dimensions
  auto input_dims = engine->getBindingDimensions(0);
  input_h = input_dims.d[2];
  input_w = input_dims.d[3];
#else
  auto input_dims = engine->getTensorShape(engine->getIOTensorName(0));
  input_h = input_dims.d[2];
  input_w = input_dims.d[3];
#endif

  // create CUDA stream
  cudaStreamCreate(&stream);

  cudaMalloc(&buffer[0], 3 * input_h * input_w * sizeof(float));
  cudaMalloc(&buffer[1], input_h * input_w * sizeof(float));

  depth_data = new float[input_h * input_w];
}

/**
 * @brief RTMSeg`s destructor
 */
DepthAnything::~DepthAnything() {
  cudaFree(stream);
  cudaFree(buffer[0]);
  cudaFree(buffer[1]);

  delete[] depth_data;
}

/**
 * @brief Network preprocessing function
 * @param image Input image
 * @return Processed Tensor
 */
std::vector<float> DepthAnything::preprocess(cv::Mat& image) {
  std::tuple<cv::Mat, int, int> resized = resize_depth(image, input_w, input_h);
  cv::Mat resized_image = std::get<0>(resized);
  std::vector<float> input_tensor;
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < resized_image.rows; i++) {
      for (int j = 0; j < resized_image.cols; j++) {
        input_tensor.emplace_back(
            ((float)resized_image.at<cv::Vec3b>(i, j)[k] - mean[k]) / std[k]);
      }
    }
  }
  return input_tensor;
}

cv::Mat DepthAnything::predict(cv::Mat& image) {
  cv::Mat clone_image;
  image.copyTo(clone_image);

  int img_w = image.cols;
  int img_h = image.rows;

  // Preprocessing
  std::vector<float> input = preprocess(clone_image);
  cudaMemcpyAsync(buffer[0], input.data(),
                  3 * input_h * input_w * sizeof(float), cudaMemcpyHostToDevice,
                  stream);

  // Inference using depth estimation model
#if NV_TENSORRT_MAJOR < 10
  context->enqueueV2(buffer, stream, nullptr);
#else
  context->executeV2(buffer);
#endif

  cudaStreamSynchronize(stream);

  // Postprocessing
  cudaMemcpyAsync(depth_data, buffer[1], input_h * input_w * sizeof(float),
                  cudaMemcpyDeviceToHost);

  // Convert the entire depth_data vector to a CV_32FC1 Mat (actual depth
  // values)
  cv::Mat depth_mat(input_h, input_w, CV_32FC1, depth_data);

  // Resize depth to match input image dimensions
  //   cv::Mat resized_depth;
  //   cv::resize(depth_mat, resized_depth, cv::Size(img_w, img_h), 0, 0,
  //              cv::INTER_LINEAR);

  return depth_mat;  // Return actual depth values (CV_32FC1)
}

void DepthAnything::build(std::string onnxPath, nvinfer1::ILogger& logger) {
  auto builder = createInferBuilder(logger);
  const auto explicitBatch =
      1U << static_cast<uint32_t>(
          NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
  INetworkDefinition* network = builder->createNetworkV2(explicitBatch);
  IBuilderConfig* config = builder->createBuilderConfig();
  if (isFP16) {
    config->setFlag(BuilderFlag::kFP16);
  }
  nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, logger);
  bool parsed = parser->parseFromFile(
      onnxPath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kINFO));
  IHostMemory* plan{builder->buildSerializedNetwork(*network, *config)};

  runtime = createInferRuntime(logger);

  engine = runtime->deserializeCudaEngine(plan->data(), plan->size());

  context = engine->createExecutionContext();

  delete network;
  delete config;
  delete parser;
  delete plan;
}

bool DepthAnything::saveEngine(const std::string& onnxpath) {
  // Create an engine path from onnx path
  std::string engine_path;
  size_t dotIndex = onnxpath.find_last_of(".");
  if (dotIndex != std::string::npos) {
    engine_path = onnxpath.substr(0, dotIndex) + ".engine";
  } else {
    return false;
  }

  // Save the engine to the path
  if (engine) {
    nvinfer1::IHostMemory* data = engine->serialize();
    std::ofstream file;
    file.open(engine_path, std::ios::binary | std::ios::out);
    if (!file.is_open()) {
      std::cout << "Create engine file" << engine_path << " failed"
                << std::endl;
      return 0;
    }
    file.write((const char*)data->data(), data->size());
    file.close();

    delete data;
  }
  return true;
}