#include "spirv_msl.hpp"
#include <fstream>
#include <iostream>
#include <vector>
using namespace spirv_cross;
int main(int argc, char **argv) {
  int failures = 0;
  bool ios = argc > 1 && std::string(argv[1]) == "ios";
  for (int i = 2; i < argc; i++) {
    try {
      std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
      auto size = in.tellg();
      in.seekg(0);
      std::vector<unsigned> words(size_t(size) / 4);
      in.read((char *)words.data(), size);
      CompilerMSL msl(std::move(words));
      auto opt = msl.get_msl_options();
      opt.platform =
          ios ? CompilerMSL::Options::iOS : CompilerMSL::Options::macOS;
      opt.set_msl_version(2, 4);
      msl.set_msl_options(opt);
      bool vertex = std::string(argv[i]).find(".vert") != std::string::npos;
      auto stage =
          vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
      auto bind = [&](unsigned set, unsigned index, unsigned buffer,
                      unsigned texture, unsigned sampler) {
        MSLResourceBinding b;
        b.stage = stage;
        b.desc_set = set;
        b.binding = index;
        b.msl_buffer = buffer;
        b.msl_texture = texture;
        b.msl_sampler = sampler;
        msl.add_msl_resource_binding(b);
      };
      for (unsigned j = 0; j < 5; j++)
        bind(1, j, j, 0, 0);
      bind(kPushConstDescSet, kPushConstBinding, 5, 0, 0);
      for (unsigned j = 0; j < 12; j++)
        bind(2, j, 0, j, j);
      bind(0, 0, 0, 12, 12);
      bind(0, 1, 0, 13, 13);
      bind(0, 3, 0, 14, 14);
      bind(0, 4, 0, 15, 0); // samplerless weather map
      bind(0, 5, 0, 16, 0); // samplerless linear scene depth
      bind(0, 2, 8, 0, 0);
      auto common = msl.get_common_options();
      common.vertex.flip_vert_y = true;
      msl.set_common_options(common);
      std::ofstream out(std::string(argv[i]) +
                        (ios ? ".ios.metal" : ".macos.metal"));
      out << msl.compile();
    } catch (std::exception &e) {
      failures++;
      std::cerr << argv[i] << ": " << e.what() << "\n";
    }
  }
  std::cout << (argc - 2 - failures) << " / " << (argc - 2)
            << " SPIR-V permutations translated to MSL2.4\n";
  return failures != 0;
}
