#include "sprite_shader_resources.h"
#include <fstream>
#include <glslang/glslang/Public/ShaderLang.h>
#include <glslang/spirv/GlslangToSpv.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  glslang::InitializeProcess();
  int failures = 0;
  for (int i = 1; i < argc; i++) {
    std::ifstream stream(argv[i]);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    std::string code = buffer.str();
    const char *text = code.c_str();
    EShLanguage stage = std::string(argv[i]).find(".vert") != std::string::npos
                            ? EShLangVertex
                            : EShLangFragment;
    glslang::TShader shader(stage);
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                       100);
    bool rt = code.find("#define SUPPORTS_RAYTRACING") != std::string::npos;
    shader.setEnvClient(glslang::EShClientVulkan,
                        rt ? glslang::EShTargetVulkan_1_2
                           : glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, rt ? glslang::EShTargetSpv_1_4
                                                  : glslang::EShTargetSpv_1_0);
    if (!shader.parse(&DefaultTBuiltInResource, 110, false,
                      EShMsgVulkanRules)) {
      std::cerr << argv[i] << "\n" << shader.getInfoLog();
      failures++;
      continue;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
      std::cerr << argv[i] << "\n" << program.getInfoLog();
      failures++;
      continue;
    }
    std::vector<unsigned int> spirv;
    glslang::SpvOptions options;
    options.optimizeSize = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    if (spirv.empty()) {
      failures++;
      continue;
    }
    std::ofstream binary(std::string(argv[i]) + ".spv", std::ios::binary);
    binary.write((const char *)spirv.data(), spirv.size() * 4);
  }
  std::cout << (argc - 1 - failures) << " / " << (argc - 1)
            << " shader permutations compiled to SPIR-V\n";
  glslang::FinalizeProcess();
  return failures != 0;
}
