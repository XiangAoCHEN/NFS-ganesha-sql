#include <iostream>
#include <string>

std::string extractFilename(const std::string &fullPath) {
  // Find the last occurrence of '/' or '\' (platform-independent)
  size_t pos = fullPath.find_last_of("/\\");

  // If the path contains no '/' or '\', return the original string
  if (pos == std::string::npos) {
    return fullPath;
  }

  // Extract the filename part starting from the character after the last '/'
  return fullPath.substr(pos + 1);
}

int main() {
  std::string fullPath = "./test/a/b/c/sbtest23.ibd";
  std::string filename = extractFilename(fullPath);
  std::cout << filename << std::endl; // Output: "sbtest23.ibd"

  return 0;
}
