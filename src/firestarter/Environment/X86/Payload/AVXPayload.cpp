#include <firestarter/Environment/X86/Payload/AVXPayload.hpp>

using namespace firestarter::environment::x86::payload;

int AVXPayload::compilePayload(std::map<std::string, unsigned> proportion) {}

std::list<std::string> AVXPayload::getAvailableInstructions(void) {}

void AVXPayload::init(unsigned long long *memoryAddr,
                      unsigned long long bufferSize) {
  X86Payload::init(memoryAddr, bufferSize, 1.654738925401e-10,
                   1.654738925401e-15);
}

void AVXPayload::highLoadFunction(...) {}
