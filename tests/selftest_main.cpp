#include <iostream>

int run_address_tests();
int run_sha256_tests();
int run_nonce_partition_tests();

int main() {
  int failures = 0;

  failures += run_address_tests();
  failures += run_sha256_tests();
  failures += run_nonce_partition_tests();

  if (failures != 0) {
    std::cerr << failures << " self-test suite failure(s)\n";
    return 1;
  }

  std::cout << "minershartx self-test passed\n";
  return 0;
}
