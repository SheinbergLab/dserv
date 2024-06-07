#include <iostream>
#include "matchdict.h"


int main(int argc, char **argv)
{
  if (argc < 3) {
    std::cout << "usage testmap pattern str" << std::endl;
    return 0;
  }
  
  MatchDict d;
  MatchSpec m(argv[1]);
  d.insert(std::string(argv[1]), m);
  
  auto is_match = d.is_match(argv[2]);

  std::cout << "is_match: " << is_match << std::endl;
  
  return 0;
}
